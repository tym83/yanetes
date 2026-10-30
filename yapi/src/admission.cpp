#include <admission.hpp>

#include <userver/formats/json/value_builder.hpp>
#include <userver/formats/serialize/common_containers.hpp>
#include <userver/utils/rand.hpp>

#include <objects.hpp>
#include <status.hpp>
#include <store.hpp>

namespace yapi::admission {

namespace json = userver::formats::json;
using json::ValueBuilder;

namespace {

constexpr std::string_view kTokenPath = "/var/run/secrets/kubernetes.io/serviceaccount";

const Resource& Core(const char* plural) { return *Registry::Builtin().Find("", "v1", plural); }

std::string Suffix() {
    static constexpr std::string_view kAlphabet = "bcdfghjklmnpqrstvwxz2456789";
    std::string s;
    for (int i = 0; i < 5; ++i) s += kAlphabet[userver::utils::RandRange(kAlphabet.size())];
    return s;
}

ApiError Forbidden(const std::string& plural, const std::string& name, const std::string& why) {
    return ApiError(403, "Forbidden", plural + " \"" + name + "\" is forbidden: " + why).About(plural, "", name);
}

// The service account plugin: the account must exist, and its token, the
// cluster CA and the namespace are mounted where clients look for them.
Value ServiceAccount(Store& store, Value pod) {
    const auto ns = objects::Namespace(pod);
    const auto spec = pod["spec"];
    const auto sa_name = spec["serviceAccountName"].As<std::string>("default");
    auto sa = store.Get(Core("serviceaccounts"), ns, sa_name);
    if (!sa) {
        throw Forbidden("pods", objects::Name(pod), "error looking up service account " + ns + "/" + sa_name + ": serviceaccount \"" + sa_name + "\" not found");
    }
    bool automount = true;
    if (spec.HasMember("automountServiceAccountToken")) {
        automount = spec["automountServiceAccountToken"].As<bool>(true);
    } else if ((*sa).HasMember("automountServiceAccountToken")) {
        automount = (*sa)["automountServiceAccountToken"].As<bool>(true);
    }
    if (!automount) return pod;

    const auto volume_name = "kube-api-access-" + Suffix();
    ValueBuilder b(pod);
    bool mounted = false;
    for (const char* list : {"initContainers", "containers"}) {
        if (!spec[list].IsArray()) continue;
        ValueBuilder out(json::Type::kArray);
        for (const auto& c : spec[list]) {
            bool has = false;
            for (const auto& m : objects::Items(c["volumeMounts"])) has = has || m["mountPath"].As<std::string>("") == kTokenPath;
            ValueBuilder cb(c);
            if (!has) {
                ValueBuilder mount(json::Type::kObject);
                mount["name"] = volume_name;
                mount["readOnly"] = true;
                mount["mountPath"] = std::string(kTokenPath);
                if (!cb.HasMember("volumeMounts")) cb["volumeMounts"] = ValueBuilder(json::Type::kArray);
                cb["volumeMounts"].PushBack(mount.ExtractValue());
                mounted = true;
            }
            out.PushBack(cb.ExtractValue());
        }
        b["spec"][list] = out;
    }
    if (!mounted) return pod;

    ValueBuilder token(json::Type::kObject);
    token["serviceAccountToken"]["expirationSeconds"] = 3607;
    token["serviceAccountToken"]["path"] = "token";
    ValueBuilder item(json::Type::kObject);
    item["key"] = "ca.crt";
    item["path"] = "ca.crt";
    ValueBuilder ca(json::Type::kObject);
    ca["configMap"]["name"] = "kube-root-ca.crt";
    ca["configMap"]["items"] = std::vector<Value>{item.ExtractValue()};
    ValueBuilder field(json::Type::kObject);
    field["path"] = "namespace";
    field["fieldRef"]["apiVersion"] = "v1";
    field["fieldRef"]["fieldPath"] = "metadata.namespace";
    ValueBuilder downward(json::Type::kObject);
    downward["downwardAPI"]["items"] = std::vector<Value>{field.ExtractValue()};
    ValueBuilder volume(json::Type::kObject);
    volume["name"] = volume_name;
    volume["projected"]["defaultMode"] = 420;
    volume["projected"]["sources"] = std::vector<Value>{token.ExtractValue(), ca.ExtractValue(), downward.ExtractValue()};
    if (!b["spec"].HasMember("volumes")) b["spec"]["volumes"] = ValueBuilder(json::Type::kArray);
    b["spec"]["volumes"].PushBack(volume.ExtractValue());
    return b.ExtractValue();
}

// DefaultTolerationSeconds: five minutes on a node that stopped answering.
Value DefaultTolerations(Value pod) {
    auto tolerations = objects::Items(pod["spec"]["tolerations"]);
    auto has = [&](const std::string& key) {
        for (const auto& t : tolerations) {
            auto effect = t["effect"].As<std::string>("");
            auto tkey = t["key"].As<std::string>("");
            bool exists_all = tkey.empty() && t["operator"].As<std::string>("") == "Exists";
            if ((tkey == key || exists_all) && (effect.empty() || effect == "NoExecute")) return true;
        }
        return false;
    };
    ValueBuilder out(tolerations);
    bool changed = false;
    for (const char* key : {"node.kubernetes.io/not-ready", "node.kubernetes.io/unreachable"}) {
        if (has(key)) continue;
        ValueBuilder t(json::Type::kObject);
        t["key"] = key;
        t["operator"] = "Exists";
        t["effect"] = "NoExecute";
        t["tolerationSeconds"] = 300;
        out.PushBack(t.ExtractValue());
        changed = true;
    }
    if (!changed) return pod;
    ValueBuilder b(pod);
    b["spec"]["tolerations"] = out;
    return b.ExtractValue();
}

// Priority: a pod's priority is its class's, or the global default's.
Value Priority(Store& store, Value pod) {
    const auto& classes = *Registry::Builtin().Find("scheduling.k8s.io", "v1", "priorityclasses");
    auto class_name = pod["spec"]["priorityClassName"].As<std::string>("");
    std::optional<Value> pc;
    if (!class_name.empty()) {
        pc = store.Get(classes, "", class_name);
        if (!pc) {
            throw Forbidden("pods", objects::Name(pod), "no PriorityClass with name " + class_name + " was found");
        }
    } else {
        for (const auto& c : store.List(classes, "", 0, "", "").items) {
            if (c["globalDefault"].As<bool>(false)) pc = c;
        }
    }
    ValueBuilder b(pod);
    if (!pc) {
        if (!pod["spec"].HasMember("priority")) b["spec"]["priority"] = 0;
        return b.ExtractValue();
    }
    auto value = (*pc)["value"].As<std::int64_t>(0);
    if (pod["spec"].HasMember("priority") && pod["spec"]["priority"].As<std::int64_t>() != value) {
        throw Forbidden("pods", objects::Name(pod), "the integer value of priority (" + std::to_string(pod["spec"]["priority"].As<std::int64_t>()) + ") must not be provided in pod spec; priority admission controller computed " + std::to_string(value) + " from the given PriorityClass name");
    }
    b["spec"]["priority"] = value;
    if (class_name.empty()) b["spec"]["priorityClassName"] = objects::Name(*pc);
    b["spec"]["preemptionPolicy"] = (*pc)["preemptionPolicy"].As<std::string>("PreemptLowerPriority");
    return b.ExtractValue();
}

// TaintNodesByCondition: a new node is not ready until it says so.
Value NodeTaints(Value node) {
    for (const auto& t : objects::Items(node["spec"]["taints"])) {
        if (t["key"].As<std::string>("") == "node.kubernetes.io/not-ready") return node;
    }
    ValueBuilder b(node);
    ValueBuilder t(json::Type::kObject);
    t["key"] = "node.kubernetes.io/not-ready";
    t["effect"] = "NoSchedule";
    if (!b["spec"].HasMember("taints")) b["spec"]["taints"] = ValueBuilder(json::Type::kArray);
    b["spec"]["taints"].PushBack(t.ExtractValue());
    return b.ExtractValue();
}

}  // namespace

Value Admit(Store& store, const Resource& r, Value object) {
    if (r.group.empty() && r.plural == "pods") {
        // Mirror pods of static pods carry their own spec and no account.
        if (object["metadata"]["annotations"].HasMember("kubernetes.io/config.mirror")) return object;
        object = Priority(store, object);
        object = DefaultTolerations(object);
        return ServiceAccount(store, object);
    }
    if (r.group.empty() && r.plural == "nodes") return NodeTaints(object);
    return object;
}

}  // namespace yapi::admission
