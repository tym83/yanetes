#include <routing.hpp>

#include <objects.hpp>
#include <registry.hpp>
#include <status.hpp>
#include <store.hpp>

namespace yapi {

Destination ServiceDestination(Store& store, const std::string& ns, const std::string& name, int port) {
    const auto& services = *Registry::Builtin().Find("", "v1", "services");
    auto service = store.Get(services, ns, name);
    if (!service) throw ApiError(503, "ServiceUnavailable", "service \"" + ns + "/" + name + "\" not found");
    Destination fallback{(*service)["spec"]["clusterIP"].As<std::string>(""), port};
    std::string port_name;
    for (const auto& p : objects::Items((*service)["spec"]["ports"])) {
        if (p["port"].As<int>(0) == port) port_name = p["name"].As<std::string>("");
    }
    const auto& slices = *Registry::Builtin().Find("discovery.k8s.io", "v1", "endpointslices");
    for (const auto& slice : store.List(slices, ns, 0, "", "").items) {
        if (slice["metadata"]["labels"]["kubernetes.io/service-name"].As<std::string>("") != name) continue;
        int target = 0;
        for (const auto& p : objects::Items(slice["ports"])) {
            if (p["name"].As<std::string>("") == port_name) target = p["port"].As<int>(0);
        }
        if (target == 0) continue;
        for (const auto& e : objects::Items(slice["endpoints"])) {
            if (!e["conditions"]["ready"].As<bool>(true)) continue;
            for (const auto& a : objects::Items(e["addresses"])) return {a.As<std::string>(""), target};
        }
    }
    return fallback;
}

}  // namespace yapi
