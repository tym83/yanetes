#include <crds.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>

#include <userver/components/component_config.hpp>
#include <userver/components/component_context.hpp>
#include <userver/engine/sleep.hpp>
#include <userver/engine/task/cancel.hpp>
#include <userver/formats/json/serialize.hpp>
#include <userver/formats/json/value_builder.hpp>
#include <userver/formats/serialize/common_containers.hpp>
#include <userver/logging/log.hpp>
#include <userver/yaml_config/merge_schemas.hpp>

#include <objects.hpp>
#include <status.hpp>
#include <store.hpp>
#include <watch.hpp>

namespace yapi {

namespace json = userver::formats::json;
using json::Value;
using json::ValueBuilder;

namespace {

constexpr std::string_view kCleanup = "customresourcecleanup.apiextensions.k8s.io";

const Resource& Definitions() {
    return *Registry::Builtin().Find("apiextensions.k8s.io", "v1", "customresourcedefinitions");
}

std::string Lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::vector<std::string> Strings(const Value& v) {
    std::vector<std::string> out;
    for (const auto& s : objects::Items(v)) out.push_back(s.As<std::string>(""));
    return out;
}

// A condition, keeping its transition time while its status holds.
Value Condition(const Value& old_conditions, const std::string& type, const std::string& status,
                const std::string& reason, const std::string& message) {
    ValueBuilder c(json::Type::kObject);
    c["type"] = type;
    c["status"] = status;
    c["reason"] = reason;
    c["message"] = message;
    c["lastTransitionTime"] = objects::Now();
    for (const auto& old : objects::Items(old_conditions)) {
        if (old["type"].As<std::string>("") == type && old["status"].As<std::string>("") == status) {
            c["lastTransitionTime"] = old["lastTransitionTime"];
        }
    }
    return c.ExtractValue();
}

}  // namespace

Crds::Crds(const userver::components::ComponentConfig& config, const userver::components::ComponentContext& context)
    : ComponentBase(config, context),
      store_(context.FindComponent<Store>()),
      watch_(context.FindComponent<Watch>()) {
    auto dir = config["openapi-dir"].As<std::string>("");
    if (!dir.empty() && std::filesystem::exists(dir + "/v3")) {
        for (const auto& entry : std::filesystem::directory_iterator(dir + "/v3")) {
            if (entry.path().extension() != ".json") continue;
            std::ifstream in(entry.path());
            std::stringstream ss;
            ss << in.rdbuf();
            Value doc;
            try {
                doc = json::FromString(ss.str());
            } catch (const std::exception&) {
                continue;
            }
            const auto schemas = doc["components"]["schemas"];
            if (!schemas.IsObject()) continue;
            for (auto it = schemas.begin(); it != schemas.end(); ++it) {
                components_.emplace(it.GetName(), *it);
                for (const auto& gvk : objects::Items((*it)["x-kubernetes-group-version-kind"])) {
                    typed_.emplace(
                        gvk["group"].As<std::string>("") + "/" + gvk["version"].As<std::string>("") + "/" +
                            gvk["kind"].As<std::string>(""),
                        it.GetName()
                    );
                }
            }
        }
    }
    LOG_INFO() << "yapi-crds: " << components_.size() << " OpenAPI schemas, " << typed_.size() << " kinds";
    for (const auto& crd : store_.List(Definitions(), "", 0, "", "").items) {
        Register(crd);
        Reconcile(crd);
    }
    tasks_.AsyncDetach("crds", [this] { Run(); });
}

Crds::~Crds() { tasks_.CancelAndWait(); }

userver::yaml_config::Schema Crds::GetStaticConfigSchema() {
    return userver::yaml_config::MergeSchemas<ComponentBase>(R"(
type: object
description: custom resource definitions
additionalProperties: false
properties:
    openapi-dir:
        type: string
        description: directory with the upstream OpenAPI documents, for the built-in types' schemas
)");
}

void Crds::Validate(const Value& crd) {
    const auto spec = crd["spec"];
    const auto name = objects::Name(crd);
    auto fail = [&](const std::string& why) {
        throw Invalid("CustomResourceDefinition", "apiextensions.k8s.io", name, why);
    };
    const auto group = spec["group"].As<std::string>("");
    const auto plural = spec["names"]["plural"].As<std::string>("");
    if (group.empty()) fail("spec.group: Required value");
    if (plural.empty()) fail("spec.names.plural: Required value");
    if (spec["names"]["kind"].As<std::string>("").empty()) fail("spec.names.kind: Required value");
    if (name != plural + "." + group) {
        fail("metadata.name: Invalid value: \"" + name + "\": must be spec.names.plural+\".\"+spec.group");
    }
    auto scope = spec["scope"].As<std::string>("");
    if (scope != "Namespaced" && scope != "Cluster") {
        fail("spec.scope: Unsupported value: \"" + scope + "\": supported values: \"Cluster\", \"Namespaced\"");
    }
    int storage = 0;
    std::size_t i = 0;
    for (const auto& v : objects::Items(spec["versions"])) {
        storage += v["storage"].As<bool>(false);
        if (!v["schema"]["openAPIV3Schema"].IsObject()) {
            fail("spec.versions[" + std::to_string(i) + "].schema.openAPIV3Schema: Required value: schemas are required");
        }
        ++i;
    }
    if (i == 0) fail("spec.versions: Invalid value: null: must have exactly one version marked as storage version");
    if (storage != 1) {
        fail("spec.versions: Invalid value: " + json::ToString(spec["versions"]) + ": must have exactly one version marked as storage version");
    }
}

void Crds::Register(const Value& crd) {
    const auto spec = crd["spec"];
    std::string storage_version;
    for (const auto& v : objects::Items(spec["versions"])) {
        if (v["storage"].As<bool>(false)) storage_version = v["name"].As<std::string>("");
    }
    std::vector<Resource> resources;
    for (const auto& v : objects::Items(spec["versions"])) {
        if (!v["served"].As<bool>(true)) continue;
        auto info = std::make_shared<CustomInfo>();
        info->crd = objects::Name(crd);
        info->storage_version = storage_version;
        info->schema = v["schema"]["openAPIV3Schema"];
        const auto sub = v["subresources"];
        info->status = sub.HasMember("status");
        info->scale = sub.HasMember("scale");
        info->spec_replicas = sub["scale"]["specReplicasPath"].As<std::string>("");
        info->status_replicas = sub["scale"]["statusReplicasPath"].As<std::string>("");
        info->label_selector = sub["scale"]["labelSelectorPath"].As<std::string>("");
        info->printer_columns = v["additionalPrinterColumns"];
        info->webhook_conversion = spec["conversion"]["strategy"].As<std::string>("None") == "Webhook";
        info->webhook = spec["conversion"]["webhook"]["clientConfig"];
        info->conversion_review_versions = spec["conversion"]["webhook"]["conversionReviewVersions"];
        const auto names = spec["names"];
        Resource r;
        r.group = spec["group"].As<std::string>("");
        r.version = v["name"].As<std::string>("");
        r.plural = names["plural"].As<std::string>("");
        r.kind = names["kind"].As<std::string>("");
        r.singular = names["singular"].As<std::string>(Lower(r.kind));
        r.namespaced = spec["scope"].As<std::string>("Namespaced") == "Namespaced";
        r.has_status = info->status;
        r.short_names = Strings(names["shortNames"]);
        r.categories = Strings(names["categories"]);
        r.custom = std::move(info);
        resources.push_back(std::move(r));
    }
    Registry::Builtin().SetCustom(objects::Name(crd), std::move(resources));
}

void Crds::Reconcile(const Value& crd) {
    const auto name = objects::Name(crd);
    if (objects::Deleting(crd)) {
        bool has_cleanup = false;
        for (const auto& f : objects::Items(crd["metadata"]["finalizers"])) has_cleanup = has_cleanup || f.As<std::string>("") == kCleanup;
        if (!has_cleanup) return;
        // Every object of the resource goes first, finalizers honored.
        const auto group = crd["spec"]["group"].As<std::string>("");
        const Resource* r = nullptr;
        for (const auto& v : objects::Items(crd["spec"]["versions"])) {
            if (!r) r = Registry::Builtin().Find(group, v["name"].As<std::string>(""), crd["spec"]["names"]["plural"].As<std::string>(""));
        }
        std::size_t left = 0;
        if (r) {
            for (const auto& item : store_.List(*r, "", 0, "", "").items) {
                ++left;
                store_.Write(*r, objects::Namespace(item), objects::Name(item), [&](const std::optional<Value>& current) {
                    if (!current) return Decision::Keep();
                    if (objects::HasFinalizers(*current)) {
                        if (objects::Deleting(*current)) return Decision::Keep();
                        ValueBuilder b(*current);
                        b["metadata"]["deletionTimestamp"] = objects::Now();
                        b["metadata"]["deletionGracePeriodSeconds"] = 0;
                        return Decision::Put(b.ExtractValue());
                    }
                    return Decision::Remove(*current);
                });
            }
        }
        if (left > 0) return;  // looked at again on the next pass
        store_.Write(Definitions(), "", name, [&](const std::optional<Value>& current) {
            if (!current) return Decision::Keep();
            ValueBuilder b(*current);
            ValueBuilder finalizers(json::Type::kArray);
            for (const auto& f : objects::Items((*current)["metadata"]["finalizers"])) {
                if (f.As<std::string>("") != kCleanup) finalizers.PushBack(f);
            }
            b["metadata"]["finalizers"] = finalizers;
            auto next = b.ExtractValue();
            return objects::HasFinalizers(next) ? Decision::Put(next) : Decision::Remove(next);
        });
        return;
    }
    store_.Write(Definitions(), "", name, [&](const std::optional<Value>& current) {
        if (!current || objects::Deleting(*current)) return Decision::Keep();
        const auto spec = (*current)["spec"];
        const auto status = (*current)["status"];
        ValueBuilder accepted(json::Type::kObject);
        accepted["plural"] = spec["names"]["plural"];
        accepted["singular"] = spec["names"]["singular"].As<std::string>(Lower(spec["names"]["kind"].As<std::string>("")));
        accepted["kind"] = spec["names"]["kind"];
        accepted["listKind"] = spec["names"]["listKind"].As<std::string>(spec["names"]["kind"].As<std::string>("") + "List");
        if (spec["names"].HasMember("shortNames")) accepted["shortNames"] = spec["names"]["shortNames"];
        if (spec["names"].HasMember("categories")) accepted["categories"] = spec["names"]["categories"];
        std::vector<std::string> stored = Strings(status["storedVersions"]);
        for (const auto& v : objects::Items(spec["versions"])) {
            auto version = v["name"].As<std::string>("");
            if (v["storage"].As<bool>(false) && std::find(stored.begin(), stored.end(), version) == stored.end()) {
                stored.push_back(version);
            }
        }
        ValueBuilder next_status(json::Type::kObject);
        next_status["acceptedNames"] = accepted;
        next_status["conditions"] = std::vector<Value>{
            Condition(status["conditions"], "NamesAccepted", "True", "NoConflicts", "no conflicts found"),
            Condition(status["conditions"], "Established", "True", "InitialNamesAccepted", "the initial names have been accepted"),
        };
        next_status["storedVersions"] = stored;
        auto desired = next_status.ExtractValue();
        if (status.IsObject() && desired == status) return Decision::Keep();
        ValueBuilder b(*current);
        b["status"] = desired;
        return Decision::Put(b.ExtractValue());
    });
}

void Crds::Run() {
    const auto& defs = Definitions();
    auto sub = watch_.Subscribe(defs, store_.Revision(defs));
    auto last_sweep = std::chrono::steady_clock::now();
    while (!userver::engine::current_task::ShouldCancel()) {
        auto e = sub->Next(std::chrono::seconds(1));
        try {
            if (e) {
                if (e->type == "DELETED") {
                    Registry::Builtin().SetCustom(objects::Name(e->object), {});
                } else {
                    Register(e->object);
                    Reconcile(e->object);
                }
            }
            if (sub->Dropped()) sub = watch_.Subscribe(defs, store_.Revision(defs));
            // Definitions being deleted wait on their objects; look again.
            if (std::chrono::steady_clock::now() - last_sweep > std::chrono::seconds(2)) {
                last_sweep = std::chrono::steady_clock::now();
                for (const auto& crd : store_.List(defs, "", 0, "", "").items) {
                    if (objects::Deleting(crd)) Reconcile(crd);
                }
            }
        } catch (const std::exception& ex) {
            if (userver::engine::current_task::ShouldCancel()) return;
            LOG_WARNING() << "yapi-crds: " << ex.what();
        }
    }
}

std::optional<schema::Schema> Crds::SchemaFor(const Resource& r) const {
    if (r.custom) {
        if (!r.custom->schema.IsObject()) return std::nullopt;
        return schema::Schema(r.custom->schema);
    }
    auto it = typed_.find(r.group + "/" + r.version + "/" + r.kind);
    if (it == typed_.end()) return std::nullopt;
    return schema::Schema(components_.at(it->second), &components_);
}

Value Crds::RootSchema(const Resource& r) const {
    if (r.custom) return r.custom->schema;
    auto it = typed_.find(r.group + "/" + r.version + "/" + r.kind);
    return it == typed_.end() ? Value() : components_.at(it->second);
}

Value Crds::Convert(const Resource& r, const Value& object, const std::string& version) const {
    if (!object.IsObject()) return object;
    ValueBuilder b(object);
    b["apiVersion"] = r.group + "/" + version;
    return b.ExtractValue();
}

}  // namespace yapi
