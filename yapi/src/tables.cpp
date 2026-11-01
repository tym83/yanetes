#include <tables.hpp>

#include <ctime>
#include <functional>
#include <set>

#include <userver/formats/json/value_builder.hpp>
#include <userver/formats/serialize/common_containers.hpp>

#include <objects.hpp>

namespace yapi::tables {

namespace json = userver::formats::json;
using json::ValueBuilder;

namespace {

struct Column {
    std::string name;
    std::string type;
    std::string format;
    int priority;
    std::function<Value(const Value&)> cell;
};

Value V(const std::string& s) { return ValueBuilder(s).ExtractValue(); }
Value V(std::int64_t n) { return ValueBuilder(n).ExtractValue(); }

std::string Str(const Value& v, const std::string& fallback = "") { return v.IsString() ? v.As<std::string>() : fallback; }
std::int64_t Int(const Value& v) { return v.IsInt64() || v.IsUInt64() || v.IsInt() ? v.As<std::int64_t>() : 0; }

std::time_t ParseTime(const std::string& s) {
    std::tm tm{};
    if (!strptime(s.c_str(), "%Y-%m-%dT%H:%M:%S", &tm)) return 0;
    return timegm(&tm);
}

std::string Since(const Value& timestamp) {
    auto s = Str(timestamp);
    if (s.empty()) return "<unknown>";
    return HumanDuration(std::time(nullptr) - ParseTime(s));
}

std::string Join(const std::vector<std::string>& parts, const std::string& sep, const std::string& empty = "<none>") {
    if (parts.empty()) return empty;
    std::string out;
    for (const auto& p : parts) out += (out.empty() ? "" : sep) + p;
    return out;
}

std::string Selector(const Value& labels) {
    std::vector<std::string> parts;
    if (labels.IsObject()) {
        for (auto it = labels.begin(); it != labels.end(); ++it) parts.push_back(it.GetName() + "=" + Str(*it));
    }
    return Join(parts, ",");
}

std::string LabelSelector(const Value& selector) {
    std::vector<std::string> parts;
    const auto match = selector["matchLabels"];
    if (match.IsObject()) {
        for (auto it = match.begin(); it != match.end(); ++it) parts.push_back(it.GetName() + "=" + Str(*it));
    }
    for (const auto& e : objects::Items(selector["matchExpressions"])) {
        std::vector<std::string> values;
        for (const auto& v : objects::Items(e["values"])) values.push_back(Str(v));
        auto op = Str(e["operator"]);
        auto key = Str(e["key"]);
        if (op == "Exists") parts.push_back(key);
        else if (op == "DoesNotExist") parts.push_back("!" + key);
        else parts.push_back(key + (op == "In" ? " in (" : " notin (") + Join(values, ",", "") + ")");
    }
    return Join(parts, ",");
}

std::string Containers(const Value& spec, const char* field) {
    std::vector<std::string> out;
    for (const auto& c : objects::Items(spec["containers"])) out.push_back(Str(c[field]));
    return Join(out, ",");
}

std::string PodStatus(const Value& pod) {
    const auto status = pod["status"];
    std::string reason = Str(status["phase"], "Unknown");
    if (!Str(status["reason"]).empty()) reason = Str(status["reason"]);
    bool initializing = false;
    int i = 0;
    auto init_total = objects::Items(pod["spec"]["initContainers"]).GetSize();
    for (const auto& c : objects::Items(status["initContainerStatuses"])) {
        const auto term = c["state"]["terminated"];
        if (term.IsObject() && Int(term["exitCode"]) == 0) {
            ++i;
            continue;
        }
        initializing = true;
        if (term.IsObject()) {
            reason = "Init:" + (Str(term["reason"]).empty() ? "ExitCode:" + std::to_string(Int(term["exitCode"])) : Str(term["reason"]));
        } else if (!Str(c["state"]["waiting"]["reason"]).empty() && Str(c["state"]["waiting"]["reason"]) != "PodInitializing") {
            reason = "Init:" + Str(c["state"]["waiting"]["reason"]);
        } else {
            reason = "Init:" + std::to_string(i) + "/" + std::to_string(init_total);
        }
        break;
    }
    if (!initializing) {
        bool running = false;
        for (const auto& c : objects::Items(status["containerStatuses"])) {
            const auto st = c["state"];
            if (!Str(st["waiting"]["reason"]).empty()) {
                reason = Str(st["waiting"]["reason"]);
            } else if (st["terminated"].IsObject()) {
                const auto t = st["terminated"];
                if (!Str(t["reason"]).empty()) reason = Str(t["reason"]);
                else if (Int(t["signal"]) != 0) reason = "Signal:" + std::to_string(Int(t["signal"]));
                else reason = "ExitCode:" + std::to_string(Int(t["exitCode"]));
            } else if (st["running"].IsObject() && c["ready"].As<bool>(false)) {
                running = true;
            }
        }
        if (reason == "Completed" && running) reason = "Running";
    }
    if (objects::Deleting(pod)) reason = Str(status["reason"]) == "NodeLost" ? "Unknown" : "Terminating";
    return reason;
}

// Enough of kubectl's JSONPath for printer columns: fields, indexes, [*]
// and [?(@.field=="value")] filters. The first match is the cell.
Value JsonPath(const Value& root, const std::string& path) {
    std::vector<Value> current{root};
    std::size_t i = 0;
    auto p = path;
    if (p.starts_with("{") && p.ends_with("}")) p = p.substr(1, p.size() - 2);
    while (i < p.size() && !current.empty()) {
        std::vector<Value> next;
        if (p[i] == '.') {
            ++i;
            std::size_t end = p.find_first_of(".[", i);
            auto field = p.substr(i, end == std::string::npos ? std::string::npos : end - i);
            i = end == std::string::npos ? p.size() : end;
            for (const auto& v : current) {
                if (v.IsObject() && v.HasMember(field)) next.push_back(v[field]);
            }
        } else if (p[i] == '[') {
            auto close = p.find(']', i);
            if (close == std::string::npos) break;
            auto inner = p.substr(i + 1, close - i - 1);
            i = close + 1;
            for (const auto& v : current) {
                if (!v.IsArray()) continue;
                if (inner == "*") {
                    for (const auto& item : v) next.push_back(item);
                } else if (inner.starts_with("?(@.")) {
                    auto eq = inner.find("==");
                    if (eq == std::string::npos) continue;
                    auto field = inner.substr(4, eq - 4);
                    auto want = inner.substr(eq + 2);
                    if (!want.empty() && want.back() == ')') want.pop_back();
                    if (want.size() >= 2 && (want.front() == '"' || want.front() == '\'')) want = want.substr(1, want.size() - 2);
                    for (const auto& item : v) {
                        if (item.IsObject() && item.HasMember(field) && item[field].IsString() && item[field].As<std::string>() == want) {
                            next.push_back(item);
                        }
                    }
                } else {
                    try {
                        auto idx = std::stol(inner);
                        if (idx < 0) idx += static_cast<long>(v.GetSize());
                        if (idx >= 0 && static_cast<std::size_t>(idx) < v.GetSize()) next.push_back(v[idx]);
                    } catch (const std::exception&) {
                    }
                }
            }
        } else {
            break;
        }
        current = std::move(next);
    }
    return current.empty() ? Value() : current.front();
}

std::vector<Column> Columns(const Resource& r) {
    const Column name{"Name", "string", "name", 0, [](const Value& o) { return V(objects::Name(o)); }};
    const Column age{"Age", "string", "", 0, [](const Value& o) { return V(Since(o["metadata"]["creationTimestamp"])); }};
    if (r.custom) {
        // A definition's printer columns, the way upstream renders them.
        std::vector<Column> cols{name};
        for (const auto& c : objects::Items(r.custom->printer_columns)) {
            auto path = c["jsonPath"].As<std::string>("");
            auto type = c["type"].As<std::string>("string");
            cols.push_back({c["name"].As<std::string>(""), type, c["format"].As<std::string>(""), c["priority"].As<int>(0),
                            [path, type](const Value& o) {
                                auto v = JsonPath(o, path);
                                if (v.IsMissing() || v.IsNull()) return Value();
                                if (type == "date" && v.IsString()) return V(Since(v));
                                return v;
                            }});
        }
        cols.push_back(age);
        return cols;
    }
    const auto& k = r.plural;
    const bool core = r.group.empty();

    if (core && k == "namespaces") {
        return {name, {"Status", "string", "", 0, [](const Value& o) { return V(Str(o["status"]["phase"])); }}, age};
    }
    if (core && k == "pods") {
        return {
            name,
            {"Ready", "string", "", 0,
             [](const Value& o) {
                 int ready = 0;
                 for (const auto& c : objects::Items(o["status"]["containerStatuses"])) ready += c["ready"].As<bool>(false);
                 return V(std::to_string(ready) + "/" + std::to_string(objects::Items(o["spec"]["containers"]).GetSize()));
             }},
            {"Status", "string", "", 0, [](const Value& o) { return V(PodStatus(o)); }},
            {"Restarts", "string", "", 0,
             [](const Value& o) {
                 std::int64_t n = 0;
                 for (const auto& c : objects::Items(o["status"]["containerStatuses"])) n += Int(c["restartCount"]);
                 return V(std::to_string(n));
             }},
            age,
            {"IP", "string", "", 1, [](const Value& o) { return V(Str(o["status"]["podIP"], "<none>")); }},
            {"Node", "string", "", 1, [](const Value& o) { return V(Str(o["spec"]["nodeName"], "<none>")); }},
            {"Nominated Node", "string", "", 1, [](const Value& o) { return V(Str(o["status"]["nominatedNodeName"], "<none>")); }},
            {"Readiness Gates", "string", "", 1, [](const Value& o) {
                 auto gates = objects::Items(o["spec"]["readinessGates"]).GetSize();
                 return V(gates ? "0/" + std::to_string(gates) : "<none>");
             }},
        };
    }
    if (core && k == "nodes") {
        return {
            name,
            {"Status", "string", "", 0,
             [](const Value& o) {
                 std::string s = "Unknown";
                 for (const auto& c : objects::Items(o["status"]["conditions"])) {
                     if (Str(c["type"]) == "Ready") s = Str(c["status"]) == "True" ? "Ready" : "NotReady";
                 }
                 if (o["spec"]["unschedulable"].As<bool>(false)) s += ",SchedulingDisabled";
                 return V(s);
             }},
            {"Roles", "string", "", 0,
             [](const Value& o) {
                 std::vector<std::string> roles;
                 const auto labels = o["metadata"]["labels"];
                 if (labels.IsObject()) {
                     for (auto it = labels.begin(); it != labels.end(); ++it) {
                         const std::string prefix = "node-role.kubernetes.io/";
                         if (it.GetName().starts_with(prefix)) roles.push_back(it.GetName().substr(prefix.size()));
                     }
                 }
                 return V(Join(roles, ","));
             }},
            age,
            {"Version", "string", "", 0, [](const Value& o) { return V(Str(o["status"]["nodeInfo"]["kubeletVersion"])); }},
            {"Internal-IP", "string", "", 1,
             [](const Value& o) {
                 for (const auto& a : objects::Items(o["status"]["addresses"])) {
                     if (Str(a["type"]) == "InternalIP") return V(Str(a["address"]));
                 }
                 return V("<none>");
             }},
            {"External-IP", "string", "", 1,
             [](const Value& o) {
                 for (const auto& a : objects::Items(o["status"]["addresses"])) {
                     if (Str(a["type"]) == "ExternalIP") return V(Str(a["address"]));
                 }
                 return V("<none>");
             }},
            {"OS-Image", "string", "", 1, [](const Value& o) { return V(Str(o["status"]["nodeInfo"]["osImage"], "<unknown>")); }},
            {"Kernel-Version", "string", "", 1, [](const Value& o) { return V(Str(o["status"]["nodeInfo"]["kernelVersion"], "<unknown>")); }},
            {"Container-Runtime", "string", "", 1, [](const Value& o) { return V(Str(o["status"]["nodeInfo"]["containerRuntimeVersion"], "<unknown>")); }},
        };
    }
    if (core && k == "services") {
        return {
            name,
            {"Type", "string", "", 0, [](const Value& o) { return V(Str(o["spec"]["type"], "ClusterIP")); }},
            {"Cluster-IP", "string", "", 0, [](const Value& o) { return V(Str(o["spec"]["clusterIP"], "<none>")); }},
            {"External-IP", "string", "", 0,
             [](const Value& o) {
                 auto type = Str(o["spec"]["type"], "ClusterIP");
                 if (type == "ExternalName") return V(Str(o["spec"]["externalName"]));
                 std::vector<std::string> ips;
                 for (const auto& ip : objects::Items(o["spec"]["externalIPs"])) ips.push_back(Str(ip));
                 if (type == "LoadBalancer") {
                     for (const auto& in : objects::Items(o["status"]["loadBalancer"]["ingress"])) {
                         ips.push_back(Str(in["ip"], Str(in["hostname"])));
                     }
                     return V(Join(ips, ",", "<pending>"));
                 }
                 return V(Join(ips, ","));
             }},
            {"Port(s)", "string", "", 0,
             [](const Value& o) {
                 std::vector<std::string> ports;
                 for (const auto& p : objects::Items(o["spec"]["ports"])) {
                     auto s = std::to_string(Int(p["port"]));
                     if (Int(p["nodePort"]) != 0) s += ":" + std::to_string(Int(p["nodePort"]));
                     ports.push_back(s + "/" + Str(p["protocol"], "TCP"));
                 }
                 return V(Join(ports, ","));
             }},
            age,
            {"Selector", "string", "", 1, [](const Value& o) { return V(Selector(o["spec"]["selector"])); }},
        };
    }
    if (core && k == "endpoints") {
        return {name,
                {"Endpoints", "string", "", 0,
                 [](const Value& o) {
                     std::vector<std::string> eps;
                     for (const auto& s : objects::Items(o["subsets"])) {
                         for (const auto& a : objects::Items(s["addresses"])) {
                             auto ports = objects::Items(s["ports"]);
                             if (ports.IsEmpty()) eps.push_back(Str(a["ip"]));
                             for (const auto& p : ports) eps.push_back(Str(a["ip"]) + ":" + std::to_string(Int(p["port"])));
                         }
                     }
                     if (eps.size() > 3) {
                         auto more = eps.size() - 3;
                         eps.resize(3);
                         return V(Join(eps, ",") + " + " + std::to_string(more) + " more...");
                     }
                     return V(Join(eps, ","));
                 }},
                age};
    }
    if (core && (k == "configmaps" || k == "secrets")) {
        auto data = Column{"Data", "integer", "", 0, [](const Value& o) {
                               std::int64_t n = 0;
                               for (const char* f : {"data", "binaryData", "stringData"}) {
                                   if (o[f].IsObject()) n += o[f].GetSize();
                               }
                               return V(n);
                           }};
        if (k == "secrets") return {name, {"Type", "string", "", 0, [](const Value& o) { return V(Str(o["type"], "Opaque")); }}, data, age};
        return {name, data, age};
    }
    if (core && k == "serviceaccounts") {
        return {name, {"Secrets", "integer", "", 0, [](const Value& o) { return V(static_cast<std::int64_t>(objects::Items(o["secrets"]).GetSize())); }}, age};
    }
    if ((core || r.group == "events.k8s.io") && k == "events") {
        return {
            {"Last Seen", "string", "", 0,
             [](const Value& o) {
                 for (const char* f : {"lastTimestamp", "eventTime", "deprecatedLastTimestamp"}) {
                     if (!Str(o[f]).empty()) return V(Since(o[f]));
                 }
                 return V(Since(o["metadata"]["creationTimestamp"]));
             }},
            {"Type", "string", "", 0, [](const Value& o) { return V(Str(o["type"])); }},
            {"Reason", "string", "", 0, [](const Value& o) { return V(Str(o["reason"])); }},
            {"Object", "string", "", 0,
             [](const Value& o) {
                 auto ref = o["involvedObject"].IsObject() ? o["involvedObject"] : o["regarding"];
                 std::string kind = Str(ref["kind"]);
                 for (auto& c : kind) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                 return V(kind + "/" + Str(ref["name"]));
             }},
            {"Message", "string", "", 0, [](const Value& o) { return V(Str(o["message"], Str(o["note"]))); }},
        };
    }
    if (r.group == "apps" && k == "deployments") {
        return {
            name,
            {"Ready", "string", "", 0, [](const Value& o) { return V(std::to_string(Int(o["status"]["readyReplicas"])) + "/" + std::to_string(Int(o["spec"]["replicas"]))); }},
            {"Up-to-date", "integer", "", 0, [](const Value& o) { return V(Int(o["status"]["updatedReplicas"])); }},
            {"Available", "integer", "", 0, [](const Value& o) { return V(Int(o["status"]["availableReplicas"])); }},
            age,
            {"Containers", "string", "", 1, [](const Value& o) { return V(Containers(o["spec"]["template"]["spec"], "name")); }},
            {"Images", "string", "", 1, [](const Value& o) { return V(Containers(o["spec"]["template"]["spec"], "image")); }},
            {"Selector", "string", "", 1, [](const Value& o) { return V(LabelSelector(o["spec"]["selector"])); }},
        };
    }
    if ((r.group == "apps" && k == "replicasets") || (core && k == "replicationcontrollers")) {
        return {
            name,
            {"Desired", "integer", "", 0, [](const Value& o) { return V(Int(o["spec"]["replicas"])); }},
            {"Current", "integer", "", 0, [](const Value& o) { return V(Int(o["status"]["replicas"])); }},
            {"Ready", "integer", "", 0, [](const Value& o) { return V(Int(o["status"]["readyReplicas"])); }},
            age,
            {"Containers", "string", "", 1, [](const Value& o) { return V(Containers(o["spec"]["template"]["spec"], "name")); }},
            {"Images", "string", "", 1, [](const Value& o) { return V(Containers(o["spec"]["template"]["spec"], "image")); }},
            {"Selector", "string", "", 1, [](const Value& o) { return V(o["spec"]["selector"].HasMember("matchLabels") ? LabelSelector(o["spec"]["selector"]) : Selector(o["spec"]["selector"])); }},
        };
    }
    if (r.group == "apps" && k == "daemonsets") {
        return {
            name,
            {"Desired", "integer", "", 0, [](const Value& o) { return V(Int(o["status"]["desiredNumberScheduled"])); }},
            {"Current", "integer", "", 0, [](const Value& o) { return V(Int(o["status"]["currentNumberScheduled"])); }},
            {"Ready", "integer", "", 0, [](const Value& o) { return V(Int(o["status"]["numberReady"])); }},
            {"Up-to-date", "integer", "", 0, [](const Value& o) { return V(Int(o["status"]["updatedNumberScheduled"])); }},
            {"Available", "integer", "", 0, [](const Value& o) { return V(Int(o["status"]["numberAvailable"])); }},
            {"Node Selector", "string", "", 0, [](const Value& o) { return V(Selector(o["spec"]["template"]["spec"]["nodeSelector"])); }},
            age,
        };
    }
    if (r.group == "apps" && k == "statefulsets") {
        return {
            name,
            {"Ready", "string", "", 0, [](const Value& o) { return V(std::to_string(Int(o["status"]["readyReplicas"])) + "/" + std::to_string(Int(o["spec"]["replicas"]))); }},
            age,
        };
    }
    if (r.group == "batch" && k == "jobs") {
        return {
            name,
            {"Status", "string", "", 0,
             [](const Value& o) {
                 // Terminal conditions win over the ones on the way there.
                 std::set<std::string> on;
                 for (const auto& c : objects::Items(o["status"]["conditions"])) {
                     if (Str(c["status"]) == "True") on.insert(Str(c["type"]));
                 }
                 for (const char* t : {"Complete", "Failed", "FailureTarget", "SuccessCriteriaMet", "Suspended"}) {
                     if (on.count(t)) return V(t);
                 }
                 return V("Running");
             }},
            {"Completions", "string", "", 0, [](const Value& o) {
                 auto c = o["spec"]["completions"];
                 return V(std::to_string(Int(o["status"]["succeeded"])) + "/" + (c.IsMissing() || c.IsNull() ? "1" : std::to_string(Int(c))));
             }},
            {"Duration", "string", "", 0, [](const Value& o) {
                 auto start = Str(o["status"]["startTime"]);
                 if (start.empty()) return V("");
                 auto end = Str(o["status"]["completionTime"]);
                 return V(HumanDuration((end.empty() ? std::time(nullptr) : ParseTime(end)) - ParseTime(start)));
             }},
            age,
        };
    }
    if (r.group == "batch" && k == "cronjobs") {
        return {
            name,
            {"Schedule", "string", "", 0, [](const Value& o) { return V(Str(o["spec"]["schedule"])); }},
            {"Timezone", "string", "", 0, [](const Value& o) { return V(Str(o["spec"]["timeZone"], "<none>")); }},
            {"Suspend", "boolean", "", 0, [](const Value& o) { return ValueBuilder(o["spec"]["suspend"].As<bool>(false)).ExtractValue(); }},
            {"Active", "integer", "", 0, [](const Value& o) { return V(static_cast<std::int64_t>(objects::Items(o["status"]["active"]).GetSize())); }},
            {"Last Schedule", "string", "", 0, [](const Value& o) { return V(Str(o["status"]["lastScheduleTime"]).empty() ? "<none>" : Since(o["status"]["lastScheduleTime"])); }},
            age,
        };
    }
    if (r.group == "coordination.k8s.io" && k == "leases") {
        return {name, {"Holder", "string", "", 0, [](const Value& o) { return V(Str(o["spec"]["holderIdentity"])); }}, age};
    }
    if (r.group == "discovery.k8s.io" && k == "endpointslices") {
        return {
            name,
            {"AddressType", "string", "", 0, [](const Value& o) { return V(Str(o["addressType"])); }},
            {"Ports", "string", "", 0, [](const Value& o) {
                 std::vector<std::string> ports;
                 for (const auto& p : objects::Items(o["ports"])) ports.push_back(std::to_string(Int(p["port"])));
                 return V(Join(ports, ",", "<unset>"));
             }},
            {"Endpoints", "string", "", 0, [](const Value& o) {
                 std::vector<std::string> eps;
                 for (const auto& e : objects::Items(o["endpoints"])) {
                     for (const auto& a : objects::Items(e["addresses"])) eps.push_back(Str(a));
                 }
                 return V(Join(eps, ",", "<unset>"));
             }},
            age,
        };
    }
    if (r.group == "networking.k8s.io" && k == "ipaddresses") {
        return {name, {"ParentRef", "string", "", 0, [](const Value& o) {
                           const auto p = o["spec"]["parentRef"];
                           return V(Str(p["resource"]) + "/" + Str(p["namespace"]) + "/" + Str(p["name"]));
                       }}};
    }
    if (r.group == "networking.k8s.io" && k == "servicecidrs") {
        return {name, {"CIDRs", "string", "", 0, [](const Value& o) {
                           std::vector<std::string> c;
                           for (const auto& v : objects::Items(o["spec"]["cidrs"])) c.push_back(Str(v));
                           return V(Join(c, ","));
                       }}, age};
    }
    if (core && (k == "persistentvolumeclaims" || k == "persistentvolumes")) {
        return {name, {"Status", "string", "", 0, [](const Value& o) { return V(Str(o["status"]["phase"])); }}, age};
    }
    // What upstream prints for kinds without a printer of their own.
    return {name, {"Created At", "date", "", 0, [](const Value& o) { return o["metadata"]["creationTimestamp"]; }}};
}

}  // namespace

std::string SelectorString(const Value& selector) {
    if (!selector.IsObject()) return "";
    if (selector.HasMember("matchLabels") || selector.HasMember("matchExpressions")) {
        auto s = LabelSelector(selector);
        return s == "<none>" ? "" : s;
    }
    auto s = Selector(selector);
    return s == "<none>" ? "" : s;
}

bool Wanted(const std::string& accept) { return accept.find("as=Table") != std::string::npos; }

bool MetadataWanted(const std::string& accept) { return accept.find("as=PartialObjectMetadata") != std::string::npos; }

Value Metadata(const Value& object) {
    ValueBuilder b(json::Type::kObject);
    b["kind"] = "PartialObjectMetadata";
    b["apiVersion"] = "meta.k8s.io/v1";
    b["metadata"] = object["metadata"];
    return b.ExtractValue();
}

std::string HumanDuration(std::int64_t s) {
    if (s < -1) return "<invalid>";
    if (s < 0) return "0s";
    auto str = [](std::int64_t v, const char* unit) { return std::to_string(v) + unit; };
    if (s < 60 * 2) return str(s, "s");
    auto m = s / 60;
    if (m < 10) return s % 60 == 0 ? str(m, "m") : str(m, "m") + str(s % 60, "s");
    if (m < 60 * 3) return str(m, "m");
    auto h = m / 60;
    if (h < 8) return m % 60 == 0 ? str(h, "h") : str(h, "h") + str(m % 60, "m");
    if (h < 48) return str(h, "h");
    if (h < 24 * 8) return h % 24 == 0 ? str(h / 24, "d") : str(h / 24, "d") + str(h % 24, "h");
    if (h < 24 * 365 * 2) return str(h / 24, "d");
    if (h < 24 * 365 * 8) {
        auto dy = (h / 24) % 365;
        return dy == 0 ? str(h / 24 / 365, "y") : str(h / 24 / 365, "y") + str(dy, "d");
    }
    return str(h / 24 / 365, "y");
}

Value Make(
    const Resource& r,
    const std::vector<Value>& items,
    bool with_columns,
    const std::string& include_object,
    const std::string& resource_version,
    const std::string& continue_token
) {
    const auto columns = Columns(r);
    ValueBuilder t(json::Type::kObject);
    t["kind"] = "Table";
    t["apiVersion"] = "meta.k8s.io/v1";
    t["metadata"] = ValueBuilder(json::Type::kObject);
    if (!resource_version.empty()) t["metadata"]["resourceVersion"] = resource_version;
    if (!continue_token.empty()) t["metadata"]["continue"] = continue_token;
    ValueBuilder defs(json::Type::kArray);
    if (with_columns) {
        for (const auto& c : columns) {
            ValueBuilder d(json::Type::kObject);
            d["name"] = c.name;
            d["type"] = c.type;
            d["format"] = c.format;
            d["description"] = "";
            d["priority"] = c.priority;
            defs.PushBack(d.ExtractValue());
        }
    }
    t["columnDefinitions"] = defs;
    ValueBuilder rows(json::Type::kArray);
    for (const auto& item : items) {
        ValueBuilder row(json::Type::kObject);
        ValueBuilder cells(json::Type::kArray);
        for (const auto& c : columns) cells.PushBack(c.cell(item));
        row["cells"] = cells;
        if (include_object == "Object") {
            row["object"] = item;
        } else if (include_object != "None") {
            row["object"] = Metadata(item);
        }
        rows.PushBack(row.ExtractValue());
    }
    t["rows"] = rows;
    return t.ExtractValue();
}

}  // namespace yapi::tables
