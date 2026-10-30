#include <objects.hpp>

#include <chrono>
#include <ctime>

#include <userver/formats/json/value_builder.hpp>
#include <userver/formats/serialize/common_containers.hpp>
#include <userver/utils/rand.hpp>
#include <userver/utils/uuid4.hpp>

#include <status.hpp>

namespace yapi::objects {

namespace json = userver::formats::json;

namespace {

std::string RandomSuffix() {
    static constexpr std::string_view kAlphabet = "bcdfghjklmnpqrstvwxz2456789";
    std::string s;
    for (int i = 0; i < 5; ++i) s += kAlphabet[userver::utils::RandRange(kAlphabet.size())];
    return s;
}

bool ValidName(const std::string& name) {
    if (name.empty() || name.size() > 253) return false;
    for (char c : name) {
        if (!(std::islower(static_cast<unsigned char>(c)) || std::isdigit(static_cast<unsigned char>(c)) || c == '-' ||
              c == '.')) {
            return false;
        }
    }
    return std::isalnum(static_cast<unsigned char>(name.front())) && std::isalnum(static_cast<unsigned char>(name.back()));
}

// A builder copied by value is a deep copy; the child builders operator[]
// returns refer to the parent, so they are taken by reference here.
void SetIfMissing(json::ValueBuilder&& b, const std::string& key, json::ValueBuilder value) {
    if (!b.HasMember(key)) b[key] = std::move(value);
}

void SetIfMissing(json::ValueBuilder& b, const std::string& key, json::ValueBuilder value) {
    if (!b.HasMember(key)) b[key] = std::move(value);
}

bool HasTag(const std::string& image) {
    auto slash = image.rfind('/');
    auto colon = image.rfind(':');
    return colon != std::string::npos && (slash == std::string::npos || colon > slash);
}

json::Value ContainerDefaults(const json::Value& container) {
    json::ValueBuilder c(container);
    SetIfMissing(c, "terminationMessagePath", "/dev/termination-log");
    SetIfMissing(c, "terminationMessagePolicy", "File");
    if (!container.HasMember("imagePullPolicy")) {
        auto image = container["image"].As<std::string>("");
        bool latest = !HasTag(image) || image.ends_with(":latest");
        c["imagePullPolicy"] = latest && image.find('@') == std::string::npos ? "Always" : "IfNotPresent";
    }
    SetIfMissing(c, "resources", json::ValueBuilder(json::Type::kObject));
    if (container["ports"].IsArray()) {
        json::ValueBuilder ports(json::Type::kArray);
        for (const auto& p : container["ports"]) {
            json::ValueBuilder pb(p);
            SetIfMissing(pb, "protocol", "TCP");
            ports.PushBack(pb.ExtractValue());
        }
        c["ports"] = ports;
    }
    return c.ExtractValue();
}

json::Value PodSpecDefaults(const json::Value& spec_value) {
    json::ValueBuilder spec(spec_value.IsObject() ? spec_value : json::ValueBuilder(json::Type::kObject).ExtractValue());
    SetIfMissing(spec, "restartPolicy", "Always");
    SetIfMissing(spec, "dnsPolicy", "ClusterFirst");
    SetIfMissing(spec, "schedulerName", "default-scheduler");
    SetIfMissing(spec, "terminationGracePeriodSeconds", 30);
    SetIfMissing(spec, "securityContext", json::ValueBuilder(json::Type::kObject));
    SetIfMissing(spec, "enableServiceLinks", true);
    auto sa = spec_value["serviceAccountName"].As<std::string>(spec_value["serviceAccount"].As<std::string>("default"));
    spec["serviceAccountName"] = sa;
    spec["serviceAccount"] = sa;
    SetIfMissing(spec, "priority", 0);
    for (const char* list : {"containers", "initContainers", "ephemeralContainers"}) {
        if (!spec_value[list].IsArray()) continue;
        json::ValueBuilder out(json::Type::kArray);
        for (const auto& c : spec_value[list]) out.PushBack(ContainerDefaults(c));
        spec[list] = out;
    }
    return spec.ExtractValue();
}

}  // namespace

std::string NewUid() { return userver::utils::generators::GenerateUuid(); }

std::string Now() {
    auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

std::string Name(const Value& object) { return object["metadata"]["name"].As<std::string>(""); }

std::string Namespace(const Value& object) { return object["metadata"]["namespace"].As<std::string>(""); }

bool HasFinalizers(const Value& object) {
    const auto f = object["metadata"]["finalizers"];
    return f.IsArray() && !f.IsEmpty();
}

bool Deleting(const Value& object) { return object["metadata"].HasMember("deletionTimestamp"); }

Value Defaults(const Resource& r, Value object) {
    json::ValueBuilder b(object);
    if (r.group.empty() && r.plural == "pods") b["spec"] = PodSpecDefaults(object["spec"]);
    if (r.group == "apps" || (r.group.empty() && r.plural == "replicationcontrollers") ||
        (r.group == "batch" && r.plural == "jobs")) {
        if (object["spec"]["template"]["spec"].IsObject()) {
            b["spec"]["template"]["spec"] = PodSpecDefaults(object["spec"]["template"]["spec"]);
        }
    }
    if (r.group == "apps" && r.plural == "deployments") {
        auto spec = b["spec"];
        SetIfMissing(spec, "replicas", 1);
        SetIfMissing(spec, "revisionHistoryLimit", 10);
        SetIfMissing(spec, "progressDeadlineSeconds", 600);
        if (!spec.HasMember("strategy")) {
            json::ValueBuilder s(json::Type::kObject);
            s["type"] = "RollingUpdate";
            s["rollingUpdate"]["maxSurge"] = "25%";
            s["rollingUpdate"]["maxUnavailable"] = "25%";
            spec["strategy"] = s;
        }
    }
    if (r.group == "apps" && r.plural == "replicasets") SetIfMissing(b["spec"], "replicas", 1);
    if (r.group.empty() && r.plural == "namespaces") {
        if (!b.HasMember("spec")) b["spec"] = json::ValueBuilder(json::Type::kObject);
        SetIfMissing(b["spec"], "finalizers", json::ValueBuilder(std::vector<std::string>{"kubernetes"}));
        if (!b.HasMember("status")) b["status"] = json::ValueBuilder(json::Type::kObject);
        SetIfMissing(b["status"], "phase", "Active");
        if (!b["metadata"].HasMember("labels")) b["metadata"]["labels"] = json::ValueBuilder(json::Type::kObject);
        b["metadata"]["labels"]["kubernetes.io/metadata.name"] = b["metadata"]["name"];
    }
    return b.ExtractValue();
}

Value PrepareCreate(const Resource& r, Value object, const std::string& ns) {
    if (!object.IsObject()) throw BadRequest("the body of the request is not an object");
    auto kind = object["kind"].As<std::string>("");
    auto api_version = object["apiVersion"].As<std::string>("");
    if (!kind.empty() && kind != r.kind) {
        throw BadRequest("the kind \"" + kind + "\" does not match the resource " + r.plural);
    }
    if (!api_version.empty() && api_version != r.GroupVersion()) {
        throw BadRequest("the API version \"" + api_version + "\" does not match the expected " + r.GroupVersion());
    }
    json::ValueBuilder b(object);
    b["kind"] = r.kind;
    b["apiVersion"] = r.GroupVersion();
    if (!b.HasMember("metadata")) b["metadata"] = json::ValueBuilder(json::Type::kObject);
    auto meta = b["metadata"];
    auto name = object["metadata"]["name"].As<std::string>("");
    auto generate = object["metadata"]["generateName"].As<std::string>("");
    if (name.empty() && !generate.empty()) name = generate + RandomSuffix();
    if (name.empty()) throw Invalid(r.kind, r.group, "", "metadata.name: Required value: name or generateName is required");
    if (!ValidName(name)) {
        throw Invalid(r.kind, r.group, name, "metadata.name: Invalid value: \"" + name + "\": a lowercase RFC 1123 subdomain must consist of lower case alphanumeric characters, '-' or '.'");
    }
    meta["name"] = name;
    if (r.namespaced) {
        auto given = object["metadata"]["namespace"].As<std::string>("");
        if (!given.empty() && given != ns) {
            throw BadRequest("the namespace of the provided object does not match the namespace sent on the request");
        }
        meta["namespace"] = ns;
    } else if (meta.HasMember("namespace")) {
        meta.Remove("namespace");
    }
    meta["uid"] = NewUid();
    meta["creationTimestamp"] = Now();
    if (meta.HasMember("resourceVersion")) meta.Remove("resourceVersion");
    if (meta.HasMember("deletionTimestamp")) meta.Remove("deletionTimestamp");
    if (r.has_status) meta["generation"] = 1;
    if (r.group.empty() && r.plural == "pods") {
        json::ValueBuilder status(json::Type::kObject);
        status["phase"] = "Pending";
        status["qosClass"] = "BestEffort";
        b["status"] = status;
    }
    return Defaults(r, b.ExtractValue());
}

Value PrepareUpdate(const Resource& r, const Value& stored, Value incoming, bool status_write) {
    if (!incoming.IsObject()) throw BadRequest("the body of the request is not an object");
    json::ValueBuilder b(status_write ? stored : incoming);
    if (status_write) {
        if (incoming.HasMember("status")) b["status"] = incoming["status"];
        // Labels and annotations travel with a status write too, as upstream allows.
        for (const char* key : {"labels", "annotations"}) {
            if (incoming["metadata"].HasMember(key)) b["metadata"][key] = incoming["metadata"][key];
        }
    } else if (r.has_status && stored.HasMember("status")) {
        b["status"] = stored["status"];
    }
    b["kind"] = r.kind;
    b["apiVersion"] = r.GroupVersion();
    auto meta = b["metadata"];
    const auto old = stored["metadata"];
    for (const char* key : {"name", "namespace", "uid", "creationTimestamp", "deletionTimestamp",
                            "deletionGracePeriodSeconds", "generation"}) {
        if (old.HasMember(key)) {
            meta[key] = old[key];
        } else if (meta.HasMember(key)) {
            meta.Remove(key);
        }
    }
    auto out = Defaults(r, b.ExtractValue());
    if (r.has_status && !status_write && out["spec"] != stored["spec"]) {
        json::ValueBuilder bumped(out);
        bumped["metadata"]["generation"] = old["generation"].As<std::int64_t>(0) + 1;
        return bumped.ExtractValue();
    }
    return out;
}

}  // namespace yapi::objects
