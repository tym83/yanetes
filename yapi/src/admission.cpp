#include <admission.hpp>

#include <userver/formats/json/value_builder.hpp>
#include <userver/formats/serialize/common_containers.hpp>
#include <userver/utils/rand.hpp>

#include <map>
#include <set>

#include <objects.hpp>
#include <quantity.hpp>
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


// What a container asks for, as a resource list: requests and limits.
using Amounts = std::map<std::string, long double>;

Amounts AmountsOf(const Value& list) {
    Amounts out;
    if (!list.IsObject()) return out;
    for (auto it = list.begin(); it != list.end(); ++it) {
        if (auto q = quantity::Parse(it->As<std::string>(""))) out[it.GetName()] = *q;
    }
    return out;
}

bool Binary(const std::string& resource) { return resource.find("memory") != std::string::npos || resource.find("storage") != std::string::npos; }

std::string Show(const std::string& resource, long double v) { return quantity::Format(v, Binary(resource)); }

// LimitRanger: defaults from the namespace's container limits, then their
// minimums, maximums and ratios.
Value LimitRanges(Store& store, const Resource& r, Value object) {
    if (!(r.group.empty() && r.plural == "pods")) return object;
    const auto ranges = store.List(*Registry::Builtin().Find("", "v1", "limitranges"), objects::Namespace(object), 0, "", "").items;
    if (ranges.empty()) return object;
    Amounts default_limits, default_requests;
    for (const auto& lr : ranges) {
        for (const auto& item : objects::Items(lr["spec"]["limits"])) {
            if (item["type"].As<std::string>("") != "Container") continue;
            for (auto [k, v] : AmountsOf(item["default"])) default_limits[k] = v;
            for (auto [k, v] : AmountsOf(item["defaultRequest"])) default_requests[k] = v;
        }
    }
    ValueBuilder b(object);
    for (const char* list : {"initContainers", "containers"}) {
        if (!object["spec"][list].IsArray()) continue;
        ValueBuilder out(json::Type::kArray);
        for (const auto& c : object["spec"][list]) {
            ValueBuilder cb(c);
            for (auto [k, v] : default_limits) {
                if (!c["resources"]["limits"].HasMember(k)) cb["resources"]["limits"][k] = Show(k, v);
            }
            for (auto [k, v] : default_requests) {
                if (!c["resources"]["requests"].HasMember(k)) cb["resources"]["requests"][k] = Show(k, v);
            }
            out.PushBack(cb.ExtractValue());
        }
        b["spec"][list] = out;
    }
    object = b.ExtractValue();
    std::vector<std::string> errors;
    for (const auto& lr : ranges) {
        for (const auto& item : objects::Items(lr["spec"]["limits"])) {
            const auto type = item["type"].As<std::string>("");
            if (type != "Container" && type != "Pod") continue;
            auto min = AmountsOf(item["min"]), max = AmountsOf(item["max"]), ratio = AmountsOf(item["maxLimitRequestRatio"]);
            auto check = [&](const Amounts& requests, const Amounts& limits) {
                for (auto [k, v] : min) {
                    if (requests.count(k) && requests.at(k) < v) errors.push_back("minimum " + k + " usage per " + type + " is " + Show(k, v) + ", but request is " + Show(k, requests.at(k)));
                    if (!requests.count(k)) errors.push_back("minimum " + k + " usage per " + type + " is " + Show(k, v) + ".  No request is specified");
                }
                for (auto [k, v] : max) {
                    if (limits.count(k) && limits.at(k) > v) errors.push_back("maximum " + k + " usage per " + type + " is " + Show(k, v) + ", but limit is " + Show(k, limits.at(k)));
                    if (!limits.count(k)) errors.push_back("maximum " + k + " usage per " + type + " is " + Show(k, v) + ".  No limit is specified");
                }
                for (auto [k, v] : ratio) {
                    if (limits.count(k) && requests.count(k) && requests.at(k) > 0 && limits.at(k) / requests.at(k) > v) {
                        errors.push_back(k + " max limit to request ratio per " + type + " is " + Show(k, v));
                    }
                }
            };
            if (type == "Container") {
                for (const auto& c : objects::Items(object["spec"]["containers"])) {
                    check(AmountsOf(c["resources"]["requests"]), AmountsOf(c["resources"]["limits"]));
                }
            } else {
                Amounts requests, limits;
                for (const auto& c : objects::Items(object["spec"]["containers"])) {
                    for (auto [k, v] : AmountsOf(c["resources"]["requests"])) requests[k] += v;
                    for (auto [k, v] : AmountsOf(c["resources"]["limits"])) limits[k] += v;
                }
                check(requests, limits);
            }
        }
    }
    if (!errors.empty()) {
        std::string msg = "[";
        for (std::size_t i = 0; i < errors.size(); ++i) msg += (i ? ", " : "") + errors[i];
        throw Forbidden("pods", objects::Name(object), msg + "]");
    }
    // Defaults may have moved the pod to another class.
    ValueBuilder qb(object);
    qb["status"]["qosClass"] = objects::Qos(object["spec"]);
    return qb.ExtractValue();
}

// What a new object adds to the usage quotas count.
Amounts Usage(const Resource& r, const Value& object) {
    Amounts u;
    const auto key = r.group.empty() ? r.plural : r.plural + "." + r.group;
    u["count/" + key] = 1;
    if (!r.group.empty()) return u;
    if (r.plural == "pods") {
        u["pods"] = 1;
        Amounts requests, limits, init_requests, init_limits;
        for (const auto& c : objects::Items(object["spec"]["containers"])) {
            for (auto [k, v] : AmountsOf(c["resources"]["requests"])) requests[k] += v;
            for (auto [k, v] : AmountsOf(c["resources"]["limits"])) limits[k] += v;
        }
        for (const auto& c : objects::Items(object["spec"]["initContainers"])) {
            for (auto [k, v] : AmountsOf(c["resources"]["requests"])) init_requests[k] = std::max(init_requests[k], v);
            for (auto [k, v] : AmountsOf(c["resources"]["limits"])) init_limits[k] = std::max(init_limits[k], v);
        }
        for (auto [k, v] : init_requests) requests[k] = std::max(requests[k], v);
        for (auto [k, v] : init_limits) limits[k] = std::max(limits[k], v);
        for (auto [k, v] : AmountsOf(object["spec"]["overhead"])) {
            requests[k] += v;
            if (limits.count(k)) limits[k] += v;
        }
        for (auto [k, v] : requests) {
            u["requests." + k] = v;
            if (k == "cpu" || k == "memory" || k == "ephemeral-storage") u[k] = v;
        }
        for (auto [k, v] : limits) u["limits." + k] = v;
    } else if (r.plural == "services") {
        u["services"] = 1;
        auto type = object["spec"]["type"].As<std::string>("ClusterIP");
        if (type == "LoadBalancer") u["services.loadbalancers"] = 1;
        if (type == "NodePort" || type == "LoadBalancer") u["services.nodeports"] = objects::Items(object["spec"]["ports"]).GetSize();
    } else if (r.plural == "persistentvolumeclaims") {
        u["persistentvolumeclaims"] = 1;
        auto storage = quantity::Parse(object["spec"]["resources"]["requests"]["storage"].As<std::string>("")).value_or(0);
        u["requests.storage"] = storage;
        auto sc = object["spec"]["storageClassName"].As<std::string>("");
        if (!sc.empty()) {
            u[sc + ".storageclass.storage.k8s.io/requests.storage"] = storage;
            u[sc + ".storageclass.storage.k8s.io/persistentvolumeclaims"] = 1;
        }
    } else if (r.plural == "configmaps" || r.plural == "secrets" || r.plural == "replicationcontrollers" ||
               r.plural == "resourcequotas") {
        u[r.plural] = 1;
    }
    return u;
}

bool ScopesMatch(const Resource& r, const Value& quota, const Value& object) {
    std::vector<std::pair<std::string, Value>> scopes;
    for (const auto& s : objects::Items(quota["spec"]["scopes"])) scopes.emplace_back(s.As<std::string>(""), Value());
    for (const auto& e : objects::Items(quota["spec"]["scopeSelector"]["matchExpressions"])) {
        scopes.emplace_back(e["scopeName"].As<std::string>(""), e);
    }
    if (scopes.empty()) return true;
    if (!(r.group.empty() && r.plural == "pods")) return false;
    const auto spec = object["spec"];
    for (const auto& [scope, expr] : scopes) {
        bool match = true;
        if (scope == "Terminating") match = spec.HasMember("activeDeadlineSeconds");
        if (scope == "NotTerminating") match = !spec.HasMember("activeDeadlineSeconds");
        if (scope == "BestEffort") match = objects::Qos(spec) == "BestEffort";
        if (scope == "NotBestEffort") match = objects::Qos(spec) != "BestEffort";
        if (scope == "PriorityClass") {
            auto pc = spec["priorityClassName"].As<std::string>("");
            auto op = expr["operator"].As<std::string>("Exists");
            bool in = false;
            for (const auto& v : objects::Items(expr["values"])) in = in || v.As<std::string>("") == pc;
            match = op == "In" ? in : op == "NotIn" ? !in : op == "Exists" ? !pc.empty() : pc.empty();
        }
        if (scope == "CrossNamespacePodAffinity") {
            bool cross = false;
            for (const char* kind : {"podAffinity", "podAntiAffinity"}) {
                for (const char* how : {"requiredDuringSchedulingIgnoredDuringExecution", "preferredDuringSchedulingIgnoredDuringExecution"}) {
                    for (const auto& t : objects::Items(spec["affinity"][kind][how])) {
                        const auto term = t.HasMember("podAffinityTerm") ? t["podAffinityTerm"] : t;
                        cross = cross || term.HasMember("namespaces") || term.HasMember("namespaceSelector");
                    }
                }
            }
            match = cross;
        }
        if (!match) return false;
    }
    return true;
}

// ResourceQuota: refuses what would go over a quota and counts what it lets in.
void Quotas(Store& store, const Resource& r, const Value& object) {
    const auto ns = objects::Namespace(object);
    if (ns.empty()) return;
    const auto& quotas_r = *Registry::Builtin().Find("", "v1", "resourcequotas");
    const auto usage = Usage(r, object);
    for (int attempt = 0; attempt < 10; ++attempt) {
        auto quotas = store.List(quotas_r, ns, 0, "", "").items;
        if (quotas.empty()) return;
        struct Charge {
            std::string name, rv;
            Amounts add;
        };
        std::vector<Charge> charges;
        for (const auto& q : quotas) {
            if (!ScopesMatch(r, q, object)) continue;
            const auto hard = AmountsOf(q["spec"]["hard"]);
            const auto status_hard = q["status"]["hard"];
            auto used = AmountsOf(q["status"]["used"]);
            Charge charge{objects::Name(q), q["metadata"]["resourceVersion"].As<std::string>(""), {}};
            std::vector<std::string> missing;
            std::string requested, using_now, limited;
            for (auto [res, limit] : hard) {
                auto it = usage.find(res);
                // A quota on pod compute makes every pod say what it needs.
                if (it == usage.end()) {
                    if (r.group.empty() && r.plural == "pods" &&
                        (res == "requests.cpu" || res == "requests.memory" || res == "limits.cpu" || res == "limits.memory" ||
                         res == "cpu" || res == "memory")) {
                        missing.push_back(res);
                    }
                    continue;
                }
                if (!status_hard.HasMember(res)) {
                    throw Forbidden(r.plural, objects::Name(object), "status unknown for quota: " + charge.name + ", resource: " + res);
                }
                if (used[res] + it->second > limit) {
                    requested += (requested.empty() ? "" : ",") + res + "=" + Show(res, it->second);
                    using_now += (using_now.empty() ? "" : ",") + res + "=" + Show(res, used[res]);
                    limited += (limited.empty() ? "" : ",") + res + "=" + Show(res, limit);
                }
                charge.add[res] = it->second;
            }
            if (!missing.empty()) {
                std::string list;
                for (const auto& m : missing) list += (list.empty() ? "" : ",") + m;
                throw Forbidden(r.plural, objects::Name(object), "failed quota: " + charge.name + ": must specify " + list);
            }
            if (!requested.empty()) {
                throw Forbidden(r.plural, objects::Name(object), "exceeded quota: " + charge.name + ", requested: " + requested + ", used: " + using_now + ", limited: " + limited);
            }
            if (!charge.add.empty()) charges.push_back(std::move(charge));
        }
        // Count the object in, unless a quota changed since it was read.
        bool raced = false;
        for (const auto& c : charges) {
            store.Write(quotas_r, ns, c.name, [&](const std::optional<Value>& current) {
                if (!current || (*current)["metadata"]["resourceVersion"].As<std::string>("") != c.rv) {
                    raced = true;
                    return Decision::Keep();
                }
                auto used = AmountsOf((*current)["status"]["used"]);
                ValueBuilder b(*current);
                for (auto [res, add] : c.add) b["status"]["used"][res] = Show(res, used[res] + add);
                return Decision::Put(b.ExtractValue());
            });
            if (raced) break;
        }
        if (!raced) return;
    }
    throw ApiError(409, "Conflict", "Operation cannot be fulfilled on resourcequotas: the quotas kept changing; please try again");
}


// RuntimeClass: the class must exist; its overhead and scheduling land on
// the pod, conflicts refused, as upstream's plugin words them.
Value RuntimeClass(Store& store, Value pod) {
    const auto name = pod["spec"]["runtimeClassName"].As<std::string>("");
    if (name.empty()) {
        if (pod["spec"].HasMember("overhead")) throw Forbidden("pods", objects::Name(pod), "pod rejected: Pod Overhead set without corresponding RuntimeClass defined Overhead");
        return pod;
    }
    auto rc = store.Get(*Registry::Builtin().Find("node.k8s.io", "v1", "runtimeclasses"), "", name);
    if (!rc) throw Forbidden("pods", objects::Name(pod), "pod rejected: RuntimeClass \"" + name + "\" not found");
    ValueBuilder b(pod);
    const auto fixed = (*rc)["overhead"]["podFixed"];
    if (fixed.IsObject()) {
        if (pod["spec"].HasMember("overhead") && pod["spec"]["overhead"] != fixed) {
            throw Forbidden("pods", objects::Name(pod), "pod rejected: Pod's Overhead doesn't match RuntimeClass's defined Overhead");
        }
        b["spec"]["overhead"] = fixed;
    } else if (pod["spec"].HasMember("overhead")) {
        throw Forbidden("pods", objects::Name(pod), "pod rejected: Pod Overhead set without corresponding RuntimeClass defined Overhead");
    }
    const auto selector = (*rc)["scheduling"]["nodeSelector"];
    if (selector.IsObject()) {
        for (auto it = selector.begin(); it != selector.end(); ++it) {
            const auto mine = pod["spec"]["nodeSelector"][it.GetName()];
            if (!mine.IsMissing() && mine != *it) {
                throw Forbidden("pods", objects::Name(pod), "conflict: runtimeClass.scheduling.nodeSelector[" + it.GetName() + "] = " +
                                                                it->As<std::string>("") + "; pod.spec.nodeSelector[" + it.GetName() + "] = " + mine.As<std::string>(""));
            }
            b["spec"]["nodeSelector"][it.GetName()] = *it;
        }
    }
    for (const auto& t : objects::Items((*rc)["scheduling"]["tolerations"])) {
        bool have = false;
        for (const auto& mine : objects::Items(pod["spec"]["tolerations"])) have = have || mine == t;
        if (have) continue;
        if (!b["spec"].HasMember("tolerations")) b["spec"]["tolerations"] = ValueBuilder(json::Type::kArray);
        b["spec"]["tolerations"].PushBack(t);
    }
    return b.ExtractValue();
}

}  // namespace

Value Admit(Store& store, const Resource& r, Value object) {
    if (r.group.empty() && r.plural == "pods") {
        // Mirror pods of static pods carry their own spec and no account.
        if (object["metadata"]["annotations"].HasMember("kubernetes.io/config.mirror")) return object;
        object = Priority(store, object);
        object = DefaultTolerations(object);
        object = RuntimeClass(store, object);
        object = LimitRanges(store, r, object);
        object = ServiceAccount(store, object);
        Quotas(store, r, object);
        return object;
    }
    if (r.group.empty() && r.plural == "nodes") return NodeTaints(object);
    if (r.namespaced && !r.CreateOnly()) Quotas(store, r, object);
    if (r.group == "apiextensions.k8s.io" && r.plural == "customresourcedefinitions") {
        // A definition's objects go before it does.
        for (const auto& f : objects::Items(object["metadata"]["finalizers"])) {
            if (f.As<std::string>("") == "customresourcecleanup.apiextensions.k8s.io") return object;
        }
        ValueBuilder b(object);
        if (!b["metadata"].HasMember("finalizers")) b["metadata"]["finalizers"] = ValueBuilder(json::Type::kArray);
        b["metadata"]["finalizers"].PushBack("customresourcecleanup.apiextensions.k8s.io");
        return b.ExtractValue();
    }
    return object;
}

}  // namespace yapi::admission
