#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <userver/clients/http/client.hpp>
#include <userver/server/handlers/http_handler_base.hpp>

#include <registry.hpp>
#include <selectors.hpp>

namespace yapi {

namespace bodies {
struct Parsed;
}
namespace kubelet {
class Connection;
}
class Auth;
class Crds;
class Webhooks;
class Aggregator;
class Policies;
class Services;
class Store;
class Watch;
struct User;

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

private:
    std::string Handle(const userver::server::http::HttpRequest& request) const;
    std::string HandleInner(const userver::server::http::HttpRequest& request) const;
    // Reviews and the like: answered, not stored.
    std::string Review(const Resource& r, const userver::server::http::HttpRequest& request, const User& user) const;
    std::string Proxy(const userver::server::http::HttpRequest& request, const Resource& r, const std::string& ns,
                      const std::string& spec, const std::string& path) const;
    std::string WebSocketLog(const userver::server::http::HttpRequest& request, std::string body) const;
    std::string Kubelet(const userver::server::http::HttpRequest& request, const std::string& ns, const std::string& name, const std::string& sub) const;
    std::pair<std::unique_ptr<kubelet::Connection>, std::string> KubeletTarget(
        const userver::server::http::HttpRequest& request, const std::string& ns, const std::string& name, const std::string& sub
    ) const;
    // Custom resources: the object as the client's version shows it, and as stored.
    userver::formats::json::Value ToView(const Resource& r, const userver::formats::json::Value& object) const;
    std::vector<userver::formats::json::Value> RemoteGroups() const;
    bool FieldsMatch(const Resource& r, const FieldSelector& fields, const userver::formats::json::Value& stored) const;
    userver::formats::json::Value ToStorage(const Resource& r, const userver::formats::json::Value& object) const;
    // Prunes and checks fields as the request's fieldValidation asks.
    userver::formats::json::Value CheckFields(const Resource& r, const userver::formats::json::Value& object,
                                              const bodies::Parsed& parsed, const userver::server::http::HttpRequest& request) const;
    void Warn(const userver::server::http::HttpRequest& request, const std::vector<std::string>& warnings) const;
    void Bind(const std::string& ns, const std::string& name, const std::string& node) const;
    void Evict(const std::string& ns, const std::string& name, const userver::formats::json::Value& options) const;
    std::string TokenRequest(const std::string& ns, const std::string& name, const userver::server::http::HttpRequest& request) const;

    Store& store_;
    Watch& watch_;
    Auth& auth_;
    Services& services_;
    Crds& crds_;
    Webhooks& webhooks_;
    Policies& policies_;
    const Aggregator& aggregator_;
    userver::clients::http::Client& http_;
    const Registry& registry_;
    std::string openapi_dir_;
};

}  // namespace yapi
