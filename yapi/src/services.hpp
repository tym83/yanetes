#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <userver/components/component_base.hpp>
#include <userver/formats/json/value.hpp>

namespace yapi {

class Store;

// Cluster IPs and node ports of services. A cluster IP is an IPAddress
// object named by the address, as upstream keeps them since 1.33; a node
// port is a row of an internal resource. Taking one is a create that fails
// when someone has it, so replicas need no shared bitmap.
class Services final : public userver::components::ComponentBase {
public:
    static constexpr std::string_view kName = "yapi-services";

    Services(const userver::components::ComponentConfig& config, const userver::components::ComponentContext& context);

    static userver::yaml_config::Schema GetStaticConfigSchema();

    // What a service write took and what it gave up. Allocation runs once per
    // write even if the store retries the transaction around it.
    struct Change {
        std::vector<std::string> ips;
        std::vector<int> ports;
        std::vector<std::string> released_ips;
        std::vector<int> released_ports;
    };

    // The service as stored: addresses carried over from the stored one,
    // missing ones taken. Throws ApiError on a taken or foreign request.
    userver::formats::json::Value Assign(
        userver::formats::json::Value service,
        const std::optional<userver::formats::json::Value>& stored,
        Change& change
    );
    // Gives back what a failed write took.
    void Undo(const Change& change, const std::string& ns, const std::string& name);
    // Gives back what a written change dropped.
    void Commit(const Change& change, const std::string& ns, const std::string& name);
    // Gives back everything of a deleted service.
    void Release(const userver::formats::json::Value& service);

    // The kubernetes service, its endpoints and the service CIDR.
    void Bootstrap();

private:
    bool TakeIp(const std::string& ip, const std::string& ns, const std::string& name);
    bool TakePort(int port, const std::string& ns, const std::string& name);
    void FreeIp(const std::string& ip, const std::string& ns, const std::string& name);
    void FreePort(int port, const std::string& ns, const std::string& name);
    std::string NextIp(const std::string& ns, const std::string& name);
    int NextPort(const std::string& ns, const std::string& name);

    Store& store_;
    std::string cidr_;
    std::uint32_t base_ = 0, size_ = 0;
    int port_min_ = 30000, port_max_ = 32767;
    std::string advertise_address_;
    int advertise_port_ = 6443;
};

}  // namespace yapi
