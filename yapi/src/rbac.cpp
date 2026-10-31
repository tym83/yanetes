#include <rbac.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

#include <userver/components/component_config.hpp>
#include <userver/components/component_context.hpp>
#include <userver/engine/task/cancel.hpp>
#include <userver/formats/json/serialize.hpp>
#include <userver/formats/json/value_builder.hpp>
#include <userver/formats/serialize/common_containers.hpp>
#include <userver/logging/log.hpp>
#include <userver/yaml_config/merge_schemas.hpp>

#include <bodies.hpp>
#include <objects.hpp>
#include <registry.hpp>
#include <store.hpp>
#include <watch.hpp>

namespace yapi {

namespace json = userver::formats::json;
using json::Value;
using json::ValueBuilder;

namespace {

constexpr const char* kPlurals[] = {"clusterroles", "roles", "clusterrolebindings", "rolebindings"};

const Resource& Kind(const std::string& plural) { return *Registry::Builtin().Find("rbac.authorization.k8s.io", "v1", plural); }

bool Has(const Value& list, const std::string& want) {
    for (const auto& v : objects::Items(list)) {
        const auto s = v.As<std::string>("");
        if (s == want || s == "*") return true;
    }
    return false;
}

// resources: "pods", "pods/log", "*", "*/scale", "pods/*".
bool ResourceMatches(const Value& rule, const Attributes& a) {
    const auto full = a.subresource.empty() ? a.resource : a.resource + "/" + a.subresource;
    for (const auto& v : objects::Items(rule["resources"])) {
        const auto r = v.As<std::string>("");
        if (r == "*" || r == full) return true;
        if (!a.subresource.empty() && r == "*/" + a.subresource) return true;
        if (!a.subresource.empty() && r == a.resource + "/*") return true;
    }
    return false;
}

bool RuleAllows(const Value& rule, const Attributes& a) {
    if (!Has(rule["verbs"], a.verb)) return false;
    if (!a.resource_request) {
        for (const auto& v : objects::Items(rule["nonResourceURLs"])) {
            const auto u = v.As<std::string>("");
            if (u == "*" || u == a.path) return true;
            if (u.ends_with("*") && a.path.starts_with(u.substr(0, u.size() - 1))) return true;
        }
        return false;
    }
    if (!Has(rule["apiGroups"], a.group) || !ResourceMatches(rule, a)) return false;
    const auto names = rule["resourceNames"];
    if (objects::Items(names).IsEmpty()) return true;
    if (a.name.empty()) return false;
    for (const auto& v : objects::Items(names)) {
        if (v.As<std::string>("") == a.name) return true;
    }
    return false;
}

bool SelectorMatches(const Value& selector, const Value& labels) {
    if (selector["matchLabels"].IsObject()) {
        for (auto it = selector["matchLabels"].begin(); it != selector["matchLabels"].end(); ++it) {
            if (labels[it.GetName()].As<std::string>("\x01") != it->As<std::string>("")) return false;
        }
    }
    for (const auto& e : objects::Items(selector["matchExpressions"])) {
        const auto key = e["key"].As<std::string>("");
        const auto op = e["operator"].As<std::string>("");
        const bool has = labels.HasMember(key);
        bool in = false;
        for (const auto& v : objects::Items(e["values"])) in = in || (has && labels[key].As<std::string>("") == v.As<std::string>(""));
        if ((op == "In" && !in) || (op == "NotIn" && in) || (op == "Exists" && !has) || (op == "DoesNotExist" && has)) return false;
    }
    return true;
}

std::string ReadFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string Quoted(const std::string& s) { return "\"" + s + "\""; }

}  // namespace

Rbac::Rbac(const userver::components::ComponentConfig& config, const userver::components::ComponentContext& context)
    : ComponentBase(config, context), store_(context.FindComponent<Store>()), watch_(context.FindComponent<Watch>()) {
    Bootstrap(config["bootstrap-dir"].As<std::string>(""));
    Refresh();
    tasks_.AsyncDetach("rbac", [this] { Run(); });
}

Rbac::~Rbac() { tasks_.CancelAndWait(); }

userver::yaml_config::Schema Rbac::GetStaticConfigSchema() {
    return userver::yaml_config::MergeSchemas<ComponentBase>(R"(
type: object
description: role-based access control
additionalProperties: false
properties:
    bootstrap-dir:
        type: string
        description: directory of the default roles and bindings, as Lists in YAML
)");
}

// Upstream's default policy, reconciled the way kube-apiserver does at
// start: what is missing is created, and objects marked for autoupdate get
// back any rules or subjects they lost.
void Rbac::Bootstrap(const std::string& dir) {
    if (dir.empty() || !std::filesystem::is_directory(dir)) return;
    std::vector<std::string> files;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.path().extension() == ".yaml") files.push_back(entry.path().string());
    }
    std::sort(files.begin(), files.end());
    std::size_t written = 0;
    for (const auto& file : files) {
        const auto list = bodies::Yaml(ReadFile(file)).value;
        for (const auto& item : objects::Items(list["items"])) {
            const auto kind = item["kind"].As<std::string>("");
            std::string plural = kind == "ClusterRole" ? "clusterroles" : kind == "Role" ? "roles"
                               : kind == "ClusterRoleBinding" ? "clusterrolebindings" : kind == "RoleBinding" ? "rolebindings" : "";
            if (plural.empty()) continue;
            const auto& r = Kind(plural);
            const auto ns = r.namespaced ? item["metadata"]["namespace"].As<std::string>("") : std::string();
            const auto name = objects::Name(item);
            const auto fresh = objects::PrepareCreate(r, item, ns);
            const char* field = plural.ends_with("bindings") ? "subjects" : "rules";
            store_.Write(r, ns, name, [&](const std::optional<Value>& current) {
                if (!current) {
                    ++written;
                    return Decision::Put(fresh);
                }
                if ((*current)["metadata"]["annotations"]["rbac.authorization.kubernetes.io/autoupdate"].As<std::string>("true") != "true") {
                    return Decision::Keep();
                }
                ValueBuilder updated(*current);
                bool changed = false;
                for (const auto& want : objects::Items(item[field])) {
                    bool present = false;
                    for (const auto& have : objects::Items((*current)[field])) present = present || have == want;
                    if (!present) {
                        if (!updated.HasMember(field) || updated[field].IsNull()) updated[field] = ValueBuilder(json::Type::kArray);
                        updated[field].PushBack(want);
                        changed = true;
                    }
                }
                if (item.HasMember("aggregationRule") && (*current)["aggregationRule"] != item["aggregationRule"]) {
                    updated["aggregationRule"] = item["aggregationRule"];
                    changed = true;
                }
                if (!changed) return Decision::Keep();
                ++written;
                return Decision::Put(objects::PrepareUpdate(r, *current, updated.ExtractValue(), false));
            });
        }
    }
    LOG_INFO() << "yapi-rbac: default policy reconciled, " << written << " objects written";
}

void Rbac::Refresh() {
    Snapshot snap;
    std::map<std::string, Value> aggregating;
    std::vector<Value> cluster_roles;
    for (const auto& role : store_.List(Kind("clusterroles"), "", 0, "", "").items) {
        snap.cluster_roles[objects::Name(role)] = std::vector<Value>(objects::Items(role["rules"]).begin(), objects::Items(role["rules"]).end());
        if (role["aggregationRule"].IsObject()) aggregating[objects::Name(role)] = role["aggregationRule"];
        cluster_roles.push_back(role);
    }
    // An aggregated role has the rules of every role its selectors pick.
    for (const auto& [name, rule] : aggregating) {
        std::vector<Value> rules;
        for (const auto& role : cluster_roles) {
            if (objects::Name(role) == name) continue;
            bool picked = false;
            for (const auto& sel : objects::Items(rule["clusterRoleSelectors"])) picked = picked || SelectorMatches(sel, role["metadata"]["labels"]);
            if (!picked) continue;
            for (const auto& r : objects::Items(role["rules"])) {
                if (std::find(rules.begin(), rules.end(), r) == rules.end()) rules.push_back(r);
            }
        }
        snap.cluster_roles[name] = std::move(rules);
    }
    for (const auto& role : store_.List(Kind("roles"), "", 0, "", "").items) {
        snap.roles[objects::Namespace(role) + "/" + objects::Name(role)] =
            std::vector<Value>(objects::Items(role["rules"]).begin(), objects::Items(role["rules"]).end());
    }
    for (const char* plural : {"clusterrolebindings", "rolebindings"}) {
        const bool cluster = std::string(plural) == "clusterrolebindings";
        for (const auto& b : store_.List(Kind(plural), "", 0, "", "").items) {
            Binding base{cluster ? "ClusterRoleBinding" : "RoleBinding", objects::Name(b), cluster ? "" : objects::Namespace(b),
                         b["roleRef"]["kind"].As<std::string>(""), b["roleRef"]["name"].As<std::string>(""), ""};
            for (const auto& s : objects::Items(b["subjects"])) {
                const auto kind = s["kind"].As<std::string>("");
                const auto name = s["name"].As<std::string>("");
                Binding binding = base;
                if (kind == "User") {
                    binding.subject = "User " + Quoted(name);
                    snap.by_subject["u:" + name].push_back(binding);
                } else if (kind == "Group") {
                    binding.subject = "Group " + Quoted(name);
                    snap.by_subject["g:" + name].push_back(binding);
                } else if (kind == "ServiceAccount") {
                    const auto sa_ns = s["namespace"].As<std::string>(base.ns);
                    binding.subject = "ServiceAccount " + Quoted(sa_ns + ":" + name);
                    snap.by_subject["u:system:serviceaccount:" + sa_ns + ":" + name].push_back(binding);
                }
            }
        }
    }
    snapshot_.Assign(std::move(snap));
}

void Rbac::Run() {
    std::vector<std::shared_ptr<Subscription>> subs;
    auto subscribe = [&] {
        subs.clear();
        for (const char* p : kPlurals) subs.push_back(watch_.Subscribe(Kind(p), store_.Revision(Kind(p))));
    };
    subscribe();
    while (!userver::engine::current_task::ShouldCancel()) {
        bool changed = false;
        for (auto& sub : subs) {
            while (sub->Next(std::chrono::milliseconds(100))) changed = true;
            if (sub->Dropped()) changed = true;
        }
        if (!changed) continue;
        try {
            for (auto& sub : subs) {
                if (sub->Dropped()) {
                    subscribe();
                    break;
                }
            }
            Refresh();
        } catch (const std::exception& e) {
            if (userver::engine::current_task::ShouldCancel()) return;
            LOG_WARNING() << "yapi-rbac: " << e.what();
        }
    }
}

std::vector<const Rbac::Binding*> Rbac::BindingsOf(const Snapshot& snap, const User& user) const {
    std::vector<const Binding*> out;
    auto add = [&](const std::string& key) {
        auto it = snap.by_subject.find(key);
        if (it == snap.by_subject.end()) return;
        for (const auto& b : it->second) out.push_back(&b);
    };
    add("u:" + user.name);
    for (const auto& g : user.groups) add("g:" + g);
    return out;
}

const std::vector<Value>* Rbac::RulesOf(const Snapshot& snap, const Binding& b) const {
    if (b.role_kind == "ClusterRole") {
        auto it = snap.cluster_roles.find(b.role_name);
        return it == snap.cluster_roles.end() ? nullptr : &it->second;
    }
    auto it = snap.roles.find(b.ns + "/" + b.role_name);
    return it == snap.roles.end() ? nullptr : &it->second;
}

Rbac::Verdict Rbac::Authorize(const User& user, const Attributes& a) const {
    if (std::find(user.groups.begin(), user.groups.end(), "system:masters") != user.groups.end()) {
        return {true, ""};
    }
    auto snap = snapshot_.Read();
    // Nodes: the system:node role, as upstream's node authorizer grants it.
    if (std::find(user.groups.begin(), user.groups.end(), "system:nodes") != user.groups.end() && user.name.starts_with("system:node:")) {
        auto it = snap->cluster_roles.find("system:node");
        if (it != snap->cluster_roles.end()) {
            for (const auto& rule : it->second) {
                if (RuleAllows(rule, a)) return {true, "allowed by node authorizer"};
            }
        }
    }
    for (const auto* b : BindingsOf(*snap, user)) {
        // A RoleBinding grants in its namespace only, and never paths.
        if (!b->ns.empty() && (!a.resource_request || a.ns != b->ns)) continue;
        const auto* rules = RulesOf(*snap, *b);
        if (!rules) continue;
        for (const auto& rule : *rules) {
            if (!RuleAllows(rule, a)) continue;
            std::string reason = "RBAC: allowed by " + b->kind + " " + Quoted(b->ns.empty() ? b->name : b->name + "/" + b->ns) + " of " +
                                 b->role_kind + " " + Quoted(b->role_name) + " to " + b->subject;
            return {true, reason};
        }
    }
    return {false, ""};
}

Rbac::Rules Rbac::RulesFor(const User& user, const std::string& ns) const {
    Rules out;
    if (std::find(user.groups.begin(), user.groups.end(), "system:masters") != user.groups.end()) {
        ValueBuilder all(json::Type::kObject);
        all["verbs"] = std::vector<std::string>{"*"};
        all["apiGroups"] = std::vector<std::string>{"*"};
        all["resources"] = std::vector<std::string>{"*"};
        out.resource.push_back(all.ExtractValue());
        ValueBuilder urls(json::Type::kObject);
        urls["verbs"] = std::vector<std::string>{"*"};
        urls["nonResourceURLs"] = std::vector<std::string>{"*"};
        out.non_resource.push_back(urls.ExtractValue());
        return out;
    }
    auto snap = snapshot_.Read();
    for (const auto* b : BindingsOf(*snap, user)) {
        if (!b->ns.empty() && b->ns != ns) continue;
        const auto* rules = RulesOf(*snap, *b);
        if (!rules) continue;
        for (const auto& rule : *rules) {
            if (rule.HasMember("nonResourceURLs")) {
                if (b->ns.empty()) out.non_resource.push_back(rule);
            } else {
                out.resource.push_back(rule);
            }
        }
    }
    return out;
}

}  // namespace yapi
