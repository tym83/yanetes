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

bool DnsSubdomain(const std::string& name, std::size_t max = 253, bool dots = true) {
    if (name.empty() || name.size() > max) return false;
    for (char c : name) {
        if (!(std::islower(static_cast<unsigned char>(c)) || std::isdigit(static_cast<unsigned char>(c)) || c == '-' ||
              (dots && c == '.'))) {
            return false;
        }
    }
    return std::isalnum(static_cast<unsigned char>(name.front())) && std::isalnum(static_cast<unsigned char>(name.back()));
}

// Names the way upstream validates them per kind; an empty string when
// the name will do, else what is wrong with it.
std::string NameProblem(const Resource& r, const std::string& name) {
    // An IP address names itself; validation checks it is one.
    if (r.group == "networking.k8s.io" && r.plural == "ipaddresses") return "";
    // Trust bundles are named signer:name:suffix.
    if (r.group == "certificates.k8s.io" && r.plural == "clustertrustbundles") {
        if (name.find('/') != std::string::npos || name.find('%') != std::string::npos) return "may not contain '/' or '%'";
        return "";
    }
    if (r.group == "rbac.authorization.k8s.io") {
        if (name == "." || name == "..") return "may not be '" + name + "'";
        if (name.find('/') != std::string::npos) return "may not contain '/'";
        if (name.find('%') != std::string::npos) return "may not contain '%'";
        return "";
    }
    if (r.group.empty() && r.plural == "namespaces") {
        return DnsSubdomain(name, 63, false) ? "" : "a lowercase RFC 1123 label must consist of lower case alphanumeric characters or '-', and must start and end with an alphanumeric character";
    }
    if (r.group.empty() && r.plural == "services") {
        bool ok = DnsSubdomain(name, 63, false) && std::islower(static_cast<unsigned char>(name.front()));
        return ok ? "" : "a DNS-1035 label must consist of lower case alphanumeric characters or '-', start with an alphabetic character, and end with an alphanumeric character";
    }
    return DnsSubdomain(name) ? "" : "a lowercase RFC 1123 subdomain must consist of lower case alphanumeric characters, '-' or '.', and must start and end with an alphanumeric character";
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

json::Value Object(const json::Value& v) { return v.IsObject() ? v : json::ValueBuilder(json::Type::kObject).ExtractValue(); }

// A handler of a probe or a lifecycle hook.
json::Value HandlerDefaults(const json::Value& handler) {
    json::ValueBuilder h(handler);
    if (handler["httpGet"].IsObject()) {
        SetIfMissing(h["httpGet"], "path", "/");
        SetIfMissing(h["httpGet"], "scheme", "HTTP");
    }
    return h.ExtractValue();
}

json::Value ProbeDefaults(const json::Value& probe) {
    json::ValueBuilder p(HandlerDefaults(probe));
    SetIfMissing(p, "timeoutSeconds", 1);
    SetIfMissing(p, "periodSeconds", 10);
    SetIfMissing(p, "successThreshold", 1);
    SetIfMissing(p, "failureThreshold", 3);
    return p.ExtractValue();
}

json::Value FieldRefDefaults(const json::Value& items) {
    json::ValueBuilder out(json::Type::kArray);
    for (const auto& item : items) {
        json::ValueBuilder ib(item);
        if (item["fieldRef"].IsObject()) SetIfMissing(ib["fieldRef"], "apiVersion", "v1");
        if (item["valueFrom"]["fieldRef"].IsObject()) SetIfMissing(ib["valueFrom"]["fieldRef"], "apiVersion", "v1");
        out.PushBack(ib.ExtractValue());
    }
    return out.ExtractValue();
}

json::Value ContainerDefaults(const json::Value& container, bool host_network) {
    json::ValueBuilder c(container);
    SetIfMissing(c, "terminationMessagePath", "/dev/termination-log");
    SetIfMissing(c, "terminationMessagePolicy", "File");
    if (!container.HasMember("imagePullPolicy")) {
        auto image = container["image"].As<std::string>("");
        bool latest = !HasTag(image) || image.ends_with(":latest");
        c["imagePullPolicy"] = latest && image.find('@') == std::string::npos ? "Always" : "IfNotPresent";
    }
    // Limits without requests ask for what they allow, as upstream defaults.
    json::ValueBuilder resources(Object(container["resources"]));
    const auto limits = container["resources"]["limits"];
    if (limits.IsObject()) {
        for (auto it = limits.begin(); it != limits.end(); ++it) {
            if (!container["resources"]["requests"].HasMember(it.GetName())) resources["requests"][it.GetName()] = *it;
        }
    }
    c["resources"] = resources;
    if (container["ports"].IsArray()) {
        json::ValueBuilder ports(json::Type::kArray);
        for (const auto& p : container["ports"]) {
            json::ValueBuilder pb(p);
            SetIfMissing(pb, "protocol", "TCP");
            if (host_network && p["hostPort"].As<int>(0) == 0) pb["hostPort"] = p["containerPort"];
            ports.PushBack(pb.ExtractValue());
        }
        c["ports"] = ports;
    }
    for (const char* probe : {"livenessProbe", "readinessProbe", "startupProbe"}) {
        if (container[probe].IsObject()) c[probe] = ProbeDefaults(container[probe]);
    }
    for (const char* hook : {"postStart", "preStop"}) {
        if (container["lifecycle"][hook].IsObject()) c["lifecycle"][hook] = HandlerDefaults(container["lifecycle"][hook]);
    }
    if (container["env"].IsArray()) c["env"] = FieldRefDefaults(container["env"]);
    return c.ExtractValue();
}

json::Value VolumeDefaults(const json::Value& volume) {
    json::ValueBuilder v(volume);
    for (const char* source : {"configMap", "secret", "downwardAPI", "projected"}) {
        if (volume[source].IsObject()) SetIfMissing(v[source], "defaultMode", 420);
    }
    if (volume["downwardAPI"]["items"].IsArray()) v["downwardAPI"]["items"] = FieldRefDefaults(volume["downwardAPI"]["items"]);
    if (volume["hostPath"].IsObject()) SetIfMissing(v["hostPath"], "type", "");
    if (volume["projected"]["sources"].IsArray()) {
        json::ValueBuilder sources(json::Type::kArray);
        for (const auto& src : volume["projected"]["sources"]) {
            json::ValueBuilder sb(src);
            if (src["serviceAccountToken"].IsObject()) SetIfMissing(sb["serviceAccountToken"], "expirationSeconds", 3600);
            if (src["downwardAPI"]["items"].IsArray()) sb["downwardAPI"]["items"] = FieldRefDefaults(src["downwardAPI"]["items"]);
            sources.PushBack(sb.ExtractValue());
        }
        v["projected"]["sources"] = sources;
    }
    return v.ExtractValue();
}

json::Value PodSpecDefaults(const json::Value& spec_value) {
    json::ValueBuilder spec(Object(spec_value));
    SetIfMissing(spec, "restartPolicy", "Always");
    SetIfMissing(spec, "dnsPolicy", "ClusterFirst");
    SetIfMissing(spec, "schedulerName", "default-scheduler");
    SetIfMissing(spec, "terminationGracePeriodSeconds", 30);
    SetIfMissing(spec, "securityContext", json::ValueBuilder(json::Type::kObject));
    SetIfMissing(spec, "enableServiceLinks", true);
    auto sa = spec_value["serviceAccountName"].As<std::string>(spec_value["serviceAccount"].As<std::string>("default"));
    spec["serviceAccountName"] = sa;
    spec["serviceAccount"] = sa;
    const bool host_network = spec_value["hostNetwork"].As<bool>(false);
    for (const char* list : {"containers", "initContainers", "ephemeralContainers"}) {
        if (!spec_value[list].IsArray()) continue;
        json::ValueBuilder out(json::Type::kArray);
        for (const auto& c : spec_value[list]) out.PushBack(ContainerDefaults(c, host_network));
        spec[list] = out;
    }
    if (spec_value["volumes"].IsArray()) {
        json::ValueBuilder out(json::Type::kArray);
        for (const auto& v : spec_value["volumes"]) out.PushBack(VolumeDefaults(v));
        spec["volumes"] = out;
    }
    return spec.ExtractValue();
}

// The QoS class upstream gives a pod from its requests and limits.
std::string QosClass(const json::Value& spec) {
    bool any = false, guaranteed = true;
    for (const char* list : {"initContainers", "containers"}) {
        if (!spec[list].IsArray()) continue;
        for (const auto& c : spec[list]) {
            const auto requests = c["resources"]["requests"];
            const auto limits = c["resources"]["limits"];
            for (const char* res : {"cpu", "memory"}) {
                bool has_request = requests.HasMember(res) && requests[res].As<std::string>("") != "0";
                bool has_limit = limits.HasMember(res);
                any = any || has_request || has_limit;
                if (!has_limit || !has_request || requests[res] != limits[res]) guaranteed = false;
            }
        }
    }
    if (!any) return "BestEffort";
    return guaranteed ? "Guaranteed" : "Burstable";
}

json::Value ServiceDefaults(const json::Value& object) {
    json::ValueBuilder b(object);
    if (!b.HasMember("spec")) b["spec"] = json::ValueBuilder(json::Type::kObject);
    auto spec = b["spec"];
    const auto in = object["spec"];
    auto type = in["type"].As<std::string>("ClusterIP");
    spec["type"] = type;
    SetIfMissing(spec, "sessionAffinity", "None");
    if (in["sessionAffinity"].As<std::string>("") == "ClientIP" &&
        in["sessionAffinityConfig"]["clientIP"]["timeoutSeconds"].IsMissing()) {
        spec["sessionAffinityConfig"]["clientIP"]["timeoutSeconds"] = 10800;
    }
    if (in["ports"].IsArray()) {
        json::ValueBuilder ports(json::Type::kArray);
        for (const auto& p : in["ports"]) {
            json::ValueBuilder pb(p);
            SetIfMissing(pb, "protocol", "TCP");
            if (type != "ExternalName" && !p.HasMember("targetPort")) pb["targetPort"] = p["port"];
            ports.PushBack(pb.ExtractValue());
        }
        spec["ports"] = ports;
    }
    if (type != "ExternalName") {
        SetIfMissing(spec, "ipFamilies", json::ValueBuilder(std::vector<std::string>{"IPv4"}));
        SetIfMissing(spec, "ipFamilyPolicy", "SingleStack");
        SetIfMissing(spec, "internalTrafficPolicy", "Cluster");
    }
    if (type == "NodePort" || type == "LoadBalancer") SetIfMissing(spec, "externalTrafficPolicy", "Cluster");
    if (type == "LoadBalancer") SetIfMissing(spec, "allocateLoadBalancerNodePorts", true);
    if (!b.HasMember("status")) b["status"]["loadBalancer"] = json::ValueBuilder(json::Type::kObject);
    return b.ExtractValue();
}

}  // namespace

std::string Qos(const Value& spec) { return QosClass(spec); }

std::string NewUid() {
    // 8-4-4-4-12, as upstream writes them.
    auto hex = userver::utils::generators::GenerateUuid();
    return hex.substr(0, 8) + "-" + hex.substr(8, 4) + "-" + hex.substr(12, 4) + "-" + hex.substr(16, 4) + "-" + hex.substr(20);
}

std::string Now() { return Time(std::chrono::system_clock::to_time_t(std::chrono::system_clock::now())); }

std::string Time(std::time_t t) {
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

Value Items(const Value& v) { return v.IsArray() ? v : json::ValueBuilder(json::Type::kArray).ExtractValue(); }

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
    if (r.group.empty() && r.plural == "services") return ServiceDefaults(object);
    if (r.group == "apps" || (r.group.empty() && r.plural == "replicationcontrollers") ||
        (r.group == "batch" && r.plural == "jobs")) {
        if (object["spec"]["template"]["spec"].IsObject()) {
            b["spec"]["template"]["spec"] = PodSpecDefaults(object["spec"]["template"]["spec"]);
        }
    }
    if (r.group == "batch" && r.plural == "cronjobs" && object["spec"]["jobTemplate"]["spec"]["template"]["spec"].IsObject()) {
        b["spec"]["jobTemplate"]["spec"]["template"]["spec"] = PodSpecDefaults(object["spec"]["jobTemplate"]["spec"]["template"]["spec"]);
    }
    if (r.group == "batch" && r.plural == "jobs") {
        auto spec = b["spec"];
        const auto in = object["spec"];
        if (!in.HasMember("parallelism")) spec["parallelism"] = 1;
        if (!in.HasMember("completions") && !in.HasMember("parallelism")) spec["completions"] = 1;
        if (!in.HasMember("backoffLimit") && !in.HasMember("backoffLimitPerIndex")) spec["backoffLimit"] = 6;
        SetIfMissing(spec, "completionMode", "NonIndexed");
        SetIfMissing(spec, "suspend", false);
        SetIfMissing(spec, "manualSelector", false);
        if (!in.HasMember("podFailurePolicy")) SetIfMissing(spec, "podReplacementPolicy", "TerminatingOrFailed");
    }
    if (r.group == "batch" && r.plural == "cronjobs") {
        auto spec = b["spec"];
        SetIfMissing(spec, "concurrencyPolicy", "Allow");
        SetIfMissing(spec, "suspend", false);
        SetIfMissing(spec, "successfulJobsHistoryLimit", 3);
        SetIfMissing(spec, "failedJobsHistoryLimit", 1);
    }
    if (r.group == "apps" && r.plural == "daemonsets") {
        auto spec = b["spec"];
        SetIfMissing(spec, "revisionHistoryLimit", 10);
        const auto strategy = object["spec"]["updateStrategy"];
        if (strategy["type"].As<std::string>("RollingUpdate") == "RollingUpdate") {
            json::ValueBuilder st(Object(strategy));
            st["type"] = "RollingUpdate";
            if (strategy["rollingUpdate"]["maxUnavailable"].IsMissing()) st["rollingUpdate"]["maxUnavailable"] = 1;
            if (strategy["rollingUpdate"]["maxSurge"].IsMissing()) st["rollingUpdate"]["maxSurge"] = 0;
            spec["updateStrategy"] = st;
        }
    }
    if (r.group == "apps" && r.plural == "statefulsets") {
        auto spec = b["spec"];
        SetIfMissing(spec, "replicas", 1);
        SetIfMissing(spec, "revisionHistoryLimit", 10);
        SetIfMissing(spec, "podManagementPolicy", "OrderedReady");
        const auto strategy = object["spec"]["updateStrategy"];
        if (strategy["type"].As<std::string>("RollingUpdate") == "RollingUpdate") {
            json::ValueBuilder st(Object(strategy));
            st["type"] = "RollingUpdate";
            if (strategy["rollingUpdate"]["partition"].IsMissing()) st["rollingUpdate"]["partition"] = 0;
            spec["updateStrategy"] = st;
        }
        json::ValueBuilder retention(Object(object["spec"]["persistentVolumeClaimRetentionPolicy"]));
        SetIfMissing(retention, "whenDeleted", "Retain");
        SetIfMissing(retention, "whenScaled", "Retain");
        spec["persistentVolumeClaimRetentionPolicy"] = retention;
        if (object["spec"]["volumeClaimTemplates"].IsArray()) {
            json::ValueBuilder claims(json::Type::kArray);
            for (const auto& c : object["spec"]["volumeClaimTemplates"]) {
                json::ValueBuilder cb(c);
                SetIfMissing(cb["spec"], "volumeMode", "Filesystem");
                claims.PushBack(cb.ExtractValue());
            }
            spec["volumeClaimTemplates"] = claims;
        }
    }
    if (r.group.empty() && r.plural == "replicationcontrollers") {
        auto spec = b["spec"];
        SetIfMissing(spec, "replicas", 1);
        const auto labels = object["spec"]["template"]["metadata"]["labels"];
        if (labels.IsObject()) {
            if (!object["spec"].HasMember("selector")) spec["selector"] = labels;
            if (!object["metadata"].HasMember("labels")) b["metadata"]["labels"] = labels;
        }
    }
    if (r.group.empty() && r.plural == "persistentvolumeclaims") {
        SetIfMissing(b["spec"], "volumeMode", "Filesystem");
        if (!object["status"].HasMember("phase")) b["status"]["phase"] = "Pending";
    }
    if (r.group.empty() && r.plural == "persistentvolumes") {
        SetIfMissing(b["spec"], "persistentVolumeReclaimPolicy", "Retain");
        SetIfMissing(b["spec"], "volumeMode", "Filesystem");
        if (!object["status"].HasMember("phase")) b["status"]["phase"] = "Pending";
    }
    if (r.group.empty() && r.plural == "secrets") SetIfMissing(b, "type", "Opaque");
    if (r.group.empty() && r.plural == "limitranges" && object["spec"]["limits"].IsArray()) {
        // Container defaults: max for the default limit, then the default
        // limit and the minimum for the default request, as upstream does.
        json::ValueBuilder limits(json::Type::kArray);
        for (const auto& item : object["spec"]["limits"]) {
            json::ValueBuilder ib(item);
            if (item["type"].As<std::string>("") == "Container") {
                auto fill = [&](const char* to, const json::Value& from) {
                    if (!from.IsObject()) return;
                    for (auto it = from.begin(); it != from.end(); ++it) {
                        if (!ib[to].HasMember(it.GetName())) ib[to][it.GetName()] = *it;
                    }
                };
                if (!ib.HasMember("default")) ib["default"] = json::ValueBuilder(json::Type::kObject);
                if (!ib.HasMember("defaultRequest")) ib["defaultRequest"] = json::ValueBuilder(json::Type::kObject);
                fill("default", item["max"]);
                // The default limit as filled so far, read from a copy: a
                // child builder cannot hand out its value.
                json::ValueBuilder defaults(item["default"].IsObject() ? item["default"] : json::ValueBuilder(json::Type::kObject).ExtractValue());
                if (item["max"].IsObject()) {
                    for (auto it = item["max"].begin(); it != item["max"].end(); ++it) {
                        if (!defaults.HasMember(it.GetName())) defaults[it.GetName()] = *it;
                    }
                }
                fill("defaultRequest", defaults.ExtractValue());
                fill("defaultRequest", item["min"]);
            }
            limits.PushBack(ib.ExtractValue());
        }
        b["spec"]["limits"] = limits;
    }
    if (r.group == "rbac.authorization.k8s.io" && object["subjects"].IsArray()) {
        json::ValueBuilder subjects(json::Type::kArray);
        for (const auto& sub : object["subjects"]) {
            json::ValueBuilder sb(sub);
            auto kind = sub["kind"].As<std::string>("");
            SetIfMissing(sb, "apiGroup", kind == "ServiceAccount" ? "" : "rbac.authorization.k8s.io");
            subjects.PushBack(sb.ExtractValue());
        }
        b["subjects"] = subjects;
    }
    if (r.group == "networking.k8s.io" && r.plural == "networkpolicies") {
        if (!object["spec"].HasMember("policyTypes")) {
            std::vector<std::string> types{"Ingress"};
            if (object["spec"].HasMember("egress")) types.push_back("Egress");
            b["spec"]["policyTypes"] = types;
        }
    }
    if (r.group == "apps" && r.plural == "deployments") {
        auto spec = b["spec"];
        SetIfMissing(spec, "replicas", 1);
        SetIfMissing(spec, "revisionHistoryLimit", 10);
        SetIfMissing(spec, "progressDeadlineSeconds", 600);
        const auto strategy = object["spec"]["strategy"];
        auto type = strategy["type"].As<std::string>("RollingUpdate");
        if (type == "RollingUpdate") {
            json::ValueBuilder s(strategy.IsObject() ? strategy : json::ValueBuilder(json::Type::kObject).ExtractValue());
            s["type"] = type;
            if (strategy["rollingUpdate"]["maxSurge"].IsMissing()) s["rollingUpdate"]["maxSurge"] = "25%";
            if (strategy["rollingUpdate"]["maxUnavailable"].IsMissing()) s["rollingUpdate"]["maxUnavailable"] = "25%";
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
    if (auto problem = NameProblem(r, name); !problem.empty()) {
        throw Invalid(r.kind, r.group, name, "metadata.name: Invalid value: \"" + name + "\": " + problem);
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
    const auto uid = NewUid();
    meta["uid"] = uid;
    meta["creationTimestamp"] = Now();
    if (meta.HasMember("resourceVersion")) meta.Remove("resourceVersion");
    if (meta.HasMember("deletionTimestamp")) meta.Remove("deletionTimestamp");
    if (r.has_status) meta["generation"] = 1;
    if (r.group == "batch" && r.plural == "jobs" && !object["spec"]["manualSelector"].As<bool>(false)) {
        // A job's pods are found by its uid, which nobody but the server knows.
        json::ValueBuilder labels(Object(object["spec"]["template"]["metadata"]["labels"]));
        labels["batch.kubernetes.io/controller-uid"] = uid;
        labels["batch.kubernetes.io/job-name"] = name;
        labels["controller-uid"] = uid;
        labels["job-name"] = name;
        b["spec"]["template"]["metadata"]["labels"] = labels;
        json::ValueBuilder selector(json::Type::kObject);
        selector["matchLabels"]["batch.kubernetes.io/controller-uid"] = uid;
        b["spec"]["selector"] = selector;
    }
    if (r.group.empty() && r.plural == "pods") {
        json::ValueBuilder status(json::Type::kObject);
        status["phase"] = "Pending";
        b["status"] = status;
        auto defaulted = Defaults(r, b.ExtractValue());
        json::ValueBuilder withQos(defaulted);
        withQos["status"]["qosClass"] = QosClass(defaulted["spec"]);
        return withQos.ExtractValue();
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
