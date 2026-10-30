#include <services.hpp>

#include <arpa/inet.h>

#include <set>

#include <userver/components/component_config.hpp>
#include <userver/components/component_context.hpp>
#include <userver/formats/json/value_builder.hpp>
#include <userver/formats/serialize/common_containers.hpp>
#include <userver/logging/log.hpp>
#include <userver/utils/rand.hpp>
#include <userver/yaml_config/merge_schemas.hpp>

#include <objects.hpp>
#include <registry.hpp>
#include <status.hpp>
#include <store.hpp>

namespace yapi {

namespace json = userver::formats::json;
using json::Value;
using json::ValueBuilder;

namespace {

// Not served: node ports are the server's own bookkeeping.
const Resource kNodePorts{"yapi.internal", "v1", "nodeports", "nodeport", "NodePort", false, false, {}, {}, {}};

constexpr int kAttempts = 64;

const Resource& Find(std::string_view group, std::string_view plural) {
    return *Registry::Builtin().Find(std::string(group), "v1", std::string(plural));
}

std::string IpString(std::uint32_t ip) {
    in_addr a{htonl(ip)};
    char buf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &a, buf, sizeof(buf));
    return buf;
}

std::optional<std::uint32_t> ParseIp(const std::string& s) {
    in_addr a{};
    if (inet_pton(AF_INET, s.c_str(), &a) != 1) return std::nullopt;
    return ntohl(a.s_addr);
}

bool Owned(const Value& object, const std::string& ns, const std::string& name) {
    const auto parent = object["spec"]["parentRef"].IsObject() ? object["spec"]["parentRef"] : object["spec"];
    return parent["namespace"].As<std::string>("") == ns && parent["name"].As<std::string>("") == name;
}

bool NeedsIp(const Value& service) { return service["spec"]["type"].As<std::string>("ClusterIP") != "ExternalName"; }

bool NeedsNodePorts(const Value& service) {
    auto type = service["spec"]["type"].As<std::string>("ClusterIP");
    return type == "NodePort" ||
           (type == "LoadBalancer" && service["spec"]["allocateLoadBalancerNodePorts"].As<bool>(true));
}

std::string PortKey(const Value& port) {
    return port["protocol"].As<std::string>("TCP") + "/" + std::to_string(port["port"].As<int>(0));
}

}  // namespace

Services::Services(const userver::components::ComponentConfig& config, const userver::components::ComponentContext& context)
    : ComponentBase(config, context),
      store_(context.FindComponent<Store>()),
      cidr_(config["service-cidr"].As<std::string>("10.96.0.0/12")),
      advertise_address_(config["advertise-address"].As<std::string>("")),
      advertise_port_(config["advertise-port"].As<int>(6443)) {
    auto slash = cidr_.find('/');
    auto base = ParseIp(cidr_.substr(0, slash));
    int bits = slash == std::string::npos ? 32 : std::stoi(cidr_.substr(slash + 1));
    if (!base || bits < 8 || bits > 30) throw std::runtime_error("yapi-services: bad service-cidr " + cidr_);
    size_ = 1u << (32 - bits);
    base_ = *base & ~(size_ - 1);
    auto range = config["node-port-range"].As<std::string>("30000-32767");
    port_min_ = std::stoi(range.substr(0, range.find('-')));
    port_max_ = std::stoi(range.substr(range.find('-') + 1));
    Bootstrap();
}

userver::yaml_config::Schema Services::GetStaticConfigSchema() {
    return userver::yaml_config::MergeSchemas<ComponentBase>(R"(
type: object
description: cluster IPs and node ports of services
additionalProperties: false
properties:
    service-cidr:
        type: string
        description: IPv4 range of cluster IPs
    node-port-range:
        type: string
        description: node ports, as first-last
    advertise-address:
        type: string
        description: the address of this server in the kubernetes service endpoints
    advertise-port:
        type: integer
        description: the port of this server in the kubernetes service endpoints
)");
}

bool Services::TakeIp(const std::string& ip, const std::string& ns, const std::string& name) {
    const auto& r = Find("networking.k8s.io", "ipaddresses");
    ValueBuilder b(json::Type::kObject);
    b["metadata"]["name"] = ip;
    b["metadata"]["labels"]["ipaddress.kubernetes.io/ip-family"] = "IPv4";
    b["metadata"]["labels"]["ipaddress.kubernetes.io/managed-by"] = "ipallocator.k8s.io";
    b["spec"]["parentRef"]["group"] = "";
    b["spec"]["parentRef"]["resource"] = "services";
    b["spec"]["parentRef"]["namespace"] = ns;
    b["spec"]["parentRef"]["name"] = name;
    auto object = objects::PrepareCreate(r, b.ExtractValue(), "");
    bool owned = false;
    auto d = store_.Write(r, "", ip, [&](const std::optional<Value>& current) {
        if (current) {
            owned = Owned(*current, ns, name);
            return Decision::Keep();
        }
        return Decision::Put(object);
    });
    return d.action == Decision::Action::kPut || owned;
}

bool Services::TakePort(int port, const std::string& ns, const std::string& name) {
    ValueBuilder b(json::Type::kObject);
    b["kind"] = kNodePorts.kind;
    b["apiVersion"] = kNodePorts.GroupVersion();
    b["metadata"]["name"] = std::to_string(port);
    b["spec"]["namespace"] = ns;
    b["spec"]["name"] = name;
    auto object = b.ExtractValue();
    bool owned = false;
    auto d = store_.Write(kNodePorts, "", std::to_string(port), [&](const std::optional<Value>& current) {
        if (current) {
            owned = Owned(*current, ns, name);
            return Decision::Keep();
        }
        return Decision::Put(object);
    });
    return d.action == Decision::Action::kPut || owned;
}

void Services::FreeIp(const std::string& ip, const std::string& ns, const std::string& name) {
    store_.Write(Find("networking.k8s.io", "ipaddresses"), "", ip, [&](const std::optional<Value>& current) {
        return current && Owned(*current, ns, name) ? Decision::Remove(*current) : Decision::Keep();
    });
}

void Services::FreePort(int port, const std::string& ns, const std::string& name) {
    store_.Write(kNodePorts, "", std::to_string(port), [&](const std::optional<Value>& current) {
        return current && Owned(*current, ns, name) ? Decision::Remove(*current) : Decision::Keep();
    });
}

std::string Services::NextIp(const std::string& ns, const std::string& name) {
    // The low band is for addresses asked for by hand (10.96.0.10 for DNS),
    // as upstream keeps it; random picks come from above it.
    std::uint32_t band = std::min<std::uint32_t>(std::max<std::uint32_t>(16, size_ / 16), 256);
    for (int i = 0; i < kAttempts; ++i) {
        auto ip = IpString(base_ + band + userver::utils::RandRange(size_ - band - 1));
        if (TakeIp(ip, ns, name)) return ip;
    }
    throw ApiError(500, "InternalError", "failed to allocate a cluster IP: the range " + cidr_ + " is full");
}

int Services::NextPort(const std::string& ns, const std::string& name) {
    for (int i = 0; i < kAttempts; ++i) {
        int port = port_min_ + static_cast<int>(userver::utils::RandRange(port_max_ - port_min_ + 1));
        if (TakePort(port, ns, name)) return port;
    }
    throw ApiError(500, "InternalError", "failed to allocate a node port: the range is full");
}

Value Services::Assign(Value service, const std::optional<Value>& stored, Change& change) {
    const auto ns = objects::Namespace(service);
    const auto name = objects::Name(service);
    // A retried transaction calls again: what the last attempt took is reused.
    std::vector<std::string> spare_ips = std::move(change.ips);
    std::vector<int> spare_ports = std::move(change.ports);
    change = Change{};

    ValueBuilder b(service);
    std::string old_ip = stored && NeedsIp(*stored) ? (*stored)["spec"]["clusterIP"].As<std::string>("") : "";
    std::string ip = service["spec"]["clusterIP"].As<std::string>("");
    if (!NeedsIp(service)) {
        if (!old_ip.empty() && old_ip != "None") change.released_ips.push_back(old_ip);
        if (b["spec"].HasMember("clusterIP")) b["spec"].Remove("clusterIP");
        if (b["spec"].HasMember("clusterIPs")) b["spec"].Remove("clusterIPs");
    } else {
        if (ip.empty()) ip = old_ip;
        if (!old_ip.empty() && ip != old_ip) {
            throw Invalid("Service", "", name, "spec.clusterIPs[0]: Invalid value: [\"" + ip + "\"]: may not change once set");
        }
        if (ip.empty()) {
            if (!spare_ips.empty()) {
                ip = spare_ips.back();
                spare_ips.pop_back();
            } else {
                ip = NextIp(ns, name);
            }
            change.ips.push_back(ip);
        } else if (ip != "None" && ip != old_ip) {
            auto parsed = ParseIp(ip);
            if (!parsed || (*parsed & ~(size_ - 1)) != base_) {
                throw Invalid("Service", "", name, "spec.clusterIPs: Invalid value: [\"" + ip + "\"]: failed to allocate IP " + ip + ": the provided IP (" + ip + ") is not in the valid range. The range of valid IPs is " + cidr_);
            }
            auto spare = std::find(spare_ips.begin(), spare_ips.end(), ip);
            if (spare != spare_ips.end()) {
                spare_ips.erase(spare);
            } else if (!TakeIp(ip, ns, name)) {
                throw Invalid("Service", "", name, "spec.clusterIPs: Invalid value: [\"" + ip + "\"]: failed to allocate IP " + ip + ": provided IP is already allocated");
            }
            change.ips.push_back(ip);
        }
        b["spec"]["clusterIP"] = ip;
        b["spec"]["clusterIPs"] = std::vector<std::string>{ip};
    }

    std::map<std::string, int> old_ports;
    std::set<int> old_set;
    if (stored && NeedsNodePorts(*stored)) {
        for (const auto& p : objects::Items((*stored)["spec"]["ports"])) {
            if (p["nodePort"].As<int>(0) == 0) continue;
            old_ports[PortKey(p)] = p["nodePort"].As<int>();
            old_set.insert(p["nodePort"].As<int>());
        }
    }
    std::set<int> kept;
    if (service["spec"]["ports"].IsArray()) {
        ValueBuilder ports(json::Type::kArray);
        int index = 0;
        for (const auto& p : service["spec"]["ports"]) {
            ValueBuilder pb(p);
            if (!NeedsNodePorts(service)) {
                if (pb.HasMember("nodePort")) pb.Remove("nodePort");
            } else {
                int np = p["nodePort"].As<int>(0);
                if (np == 0 && old_ports.count(PortKey(p))) np = old_ports[PortKey(p)];
                if (np == 0) {
                    if (!spare_ports.empty()) {
                        np = spare_ports.back();
                        spare_ports.pop_back();
                    } else {
                        np = NextPort(ns, name);
                    }
                    change.ports.push_back(np);
                } else if (!old_set.count(np) && !kept.count(np)) {
                    if (np < port_min_ || np > port_max_) {
                        throw Invalid("Service", "", name, "spec.ports[" + std::to_string(index) + "].nodePort: Invalid value: " + std::to_string(np) + ": provided port is not in the valid range. The range of valid ports is " + std::to_string(port_min_) + "-" + std::to_string(port_max_));
                    }
                    auto spare = std::find(spare_ports.begin(), spare_ports.end(), np);
                    if (spare != spare_ports.end()) {
                        spare_ports.erase(spare);
                    } else if (!TakePort(np, ns, name)) {
                        throw Invalid("Service", "", name, "spec.ports[" + std::to_string(index) + "].nodePort: Invalid value: " + std::to_string(np) + ": provided port is already allocated");
                    }
                    change.ports.push_back(np);
                }
                pb["nodePort"] = np;
                kept.insert(np);
            }
            ports.PushBack(pb.ExtractValue());
            ++index;
        }
        b["spec"]["ports"] = ports;
    }
    for (int p : old_set) {
        if (!kept.count(p)) change.released_ports.push_back(p);
    }
    // What the last attempt took and this one did not need goes back.
    for (const auto& s : spare_ips) FreeIp(s, ns, name);
    for (int s : spare_ports) FreePort(s, ns, name);
    return b.ExtractValue();
}

void Services::Undo(const Change& change, const std::string& ns, const std::string& name) {
    for (const auto& ip : change.ips) FreeIp(ip, ns, name);
    for (int p : change.ports) FreePort(p, ns, name);
}

void Services::Commit(const Change& change, const std::string& ns, const std::string& name) {
    for (const auto& ip : change.released_ips) FreeIp(ip, ns, name);
    for (int p : change.released_ports) FreePort(p, ns, name);
}

void Services::Release(const Value& service) {
    const auto ns = objects::Namespace(service);
    const auto name = objects::Name(service);
    auto ip = service["spec"]["clusterIP"].As<std::string>("");
    if (!ip.empty() && ip != "None") FreeIp(ip, ns, name);
    for (const auto& p : objects::Items(service["spec"]["ports"])) {
        if (p["nodePort"].As<int>(0) != 0) FreePort(p["nodePort"].As<int>(), ns, name);
    }
}

void Services::Bootstrap() {
    // Writes the object unless what is stored already says the same.
    auto ensure = [&](const Resource& r, const std::string& ns, Value desired) {
        auto name = objects::Name(desired);
        store_.Write(r, ns, name, [&](const std::optional<Value>& current) {
            if (!current) return Decision::Put(objects::PrepareCreate(r, desired, ns));
            auto next = objects::PrepareUpdate(r, *current, desired, false);
            if (r.has_status && desired.HasMember("status")) {
                ValueBuilder nb(next);
                nb["status"] = desired["status"];
                next = nb.ExtractValue();
            }
            ValueBuilder nb(next);
            nb["metadata"]["resourceVersion"] = (*current)["metadata"]["resourceVersion"];
            next = nb.ExtractValue();
            return next == *current ? Decision::Keep() : Decision::Put(next);
        });
    };

    ValueBuilder cidr(json::Type::kObject);
    cidr["metadata"]["name"] = "kubernetes";
    cidr["spec"]["cidrs"] = std::vector<std::string>{cidr_};
    ValueBuilder ready(json::Type::kObject);
    ready["type"] = "Ready";
    ready["status"] = "True";
    ready["reason"] = "";
    ready["message"] = "Kubernetes Service CIDR is ready";
    ready["lastTransitionTime"] = objects::Now();
    cidr["status"]["conditions"] = std::vector<Value>{ready.ExtractValue()};
    ensure(Find("networking.k8s.io", "servicecidrs"), "", cidr.ExtractValue());

    const auto kubernetes_ip = IpString(base_ + 1);
    TakeIp(kubernetes_ip, "default", "kubernetes");

    ValueBuilder port(json::Type::kObject);
    port["name"] = "https";
    port["protocol"] = "TCP";
    port["port"] = 443;
    port["targetPort"] = advertise_port_;
    ValueBuilder svc(json::Type::kObject);
    svc["metadata"]["name"] = "kubernetes";
    svc["metadata"]["labels"]["component"] = "apiserver";
    svc["metadata"]["labels"]["provider"] = "kubernetes";
    svc["spec"]["type"] = "ClusterIP";
    svc["spec"]["clusterIP"] = kubernetes_ip;
    svc["spec"]["clusterIPs"] = std::vector<std::string>{kubernetes_ip};
    svc["spec"]["ports"] = std::vector<Value>{port.ExtractValue()};
    svc["spec"]["sessionAffinity"] = "None";
    ensure(Find("", "services"), "default", svc.ExtractValue());

    if (advertise_address_.empty()) {
        LOG_WARNING() << "yapi-services: no advertise-address; the kubernetes service has no endpoints";
        return;
    }
    ValueBuilder epport(json::Type::kObject);
    epport["name"] = "https";
    epport["port"] = advertise_port_;
    epport["protocol"] = "TCP";
    auto ep_port = epport.ExtractValue();

    ValueBuilder addr(json::Type::kObject);
    addr["ip"] = advertise_address_;
    ValueBuilder subset(json::Type::kObject);
    subset["addresses"] = std::vector<Value>{addr.ExtractValue()};
    subset["ports"] = std::vector<Value>{ep_port};
    ValueBuilder ep(json::Type::kObject);
    ep["metadata"]["name"] = "kubernetes";
    ep["metadata"]["labels"]["endpointslice.kubernetes.io/skip-mirror"] = "true";
    ep["subsets"] = std::vector<Value>{subset.ExtractValue()};
    ensure(Find("", "endpoints"), "default", ep.ExtractValue());

    ValueBuilder endpoint(json::Type::kObject);
    endpoint["addresses"] = std::vector<std::string>{advertise_address_};
    endpoint["conditions"]["ready"] = true;
    ValueBuilder slice(json::Type::kObject);
    slice["metadata"]["name"] = "kubernetes";
    slice["metadata"]["labels"]["kubernetes.io/service-name"] = "kubernetes";
    slice["addressType"] = "IPv4";
    slice["endpoints"] = std::vector<Value>{endpoint.ExtractValue()};
    slice["ports"] = std::vector<Value>{ep_port};
    ensure(Find("discovery.k8s.io", "endpointslices"), "default", slice.ExtractValue());
    LOG_INFO() << "yapi-services: kubernetes service at " << kubernetes_ip << " -> " << advertise_address_;
}

}  // namespace yapi
