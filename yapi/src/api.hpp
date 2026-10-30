#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include <userver/server/handlers/http_handler_base.hpp>

#include <registry.hpp>

namespace yapi {

class Store;
class Watch;

// Everything under /api, /apis and the server's own endpoints. One class,
// registered once per path depth: userver matches whole segments.
class Api final : public userver::server::handlers::HttpHandlerBase {
public:
    static constexpr std::string_view kName = "yapi-api";

    Api(const userver::components::ComponentConfig& config, const userver::components::ComponentContext& context);

    static userver::yaml_config::Schema GetStaticConfigSchema();

    bool IsStreamed(const userver::server::http::HttpRequest& request, userver::server::request::RequestContext&)
        const override;

    std::string HandleRequestThrow(
        const userver::server::http::HttpRequest& request,
        userver::server::request::RequestContext& context
    ) const override;

    void HandleStreamRequest(
        userver::server::http::HttpRequest& request,
        userver::server::request::RequestContext& context,
        userver::server::http::ResponseBodyStream& stream
    ) const override;

    struct User {
        std::string name;
        std::string uid;
        std::vector<std::string> groups;
    };

private:
    std::string Handle(const userver::server::http::HttpRequest& request) const;
    const User* Authenticate(const userver::server::http::HttpRequest& request) const;

    Store& store_;
    Watch& watch_;
    const Registry& registry_;
    std::unordered_map<std::string, User> tokens_;
    std::string openapi_dir_;
};

}  // namespace yapi
