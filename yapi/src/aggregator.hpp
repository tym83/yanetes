#pragma once

#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <userver/clients/http/client.hpp>
#include <userver/components/component_base.hpp>
#include <userver/engine/mutex.hpp>
#include <userver/formats/json/value.hpp>
#include <userver/server/http/http_request.hpp>

#include <auth.hpp>

namespace yapi {

class Store;

// API groups other servers serve, as APIServices register them: requests
// for them go to the service named, with the caller's identity in the
// front-proxy headers, and their resources show in discovery.
class Aggregator final : public userver::components::ComponentBase {
public:
    static constexpr std::string_view kName = "yapi-aggregator";

    Aggregator(const userver::components::ComponentConfig& config, const userver::components::ComponentContext& context);

    static userver::yaml_config::Schema GetStaticConfigSchema();

    struct Remote {
        std::string group, version;
        userver::formats::json::Value apiservice;
    };

    // The APIService serving a group version from elsewhere, if one does.
    std::optional<Remote> Find(const std::string& group, const std::string& version) const;

    // Every group version served elsewhere, highest priority first within
    // a group, groups by their minimum priority.
    std::vector<Remote> All() const;

    // Sends the request on and writes the answer into its response.
    std::string Forward(const userver::server::http::HttpRequest& request, const Remote& remote, const User& user) const;

    // The remote server's APIResourceList for its group version, fetched
    // now and then.
    std::optional<userver::formats::json::Value> Resources(const Remote& remote) const;

private:
    std::shared_ptr<userver::clients::http::Response> Send(
        const Remote& remote,
        const std::string& method,
        const std::string& path_and_query,
        const std::string& body,
        const std::vector<std::pair<std::string, std::string>>& headers,
        const User* user
    ) const;
    void PublishAuthentication(const std::string& client_ca, const std::string& requestheader_ca) const;

    Store& store_;
    userver::clients::http::Client& http_;
    std::string proxy_cert_, proxy_key_;

    struct Cached {
        std::chrono::steady_clock::time_point at;
        userver::formats::json::Value resources;
    };
    mutable userver::engine::Mutex mutex_;
    mutable std::map<std::string, Cached> cache_;
};

}  // namespace yapi
