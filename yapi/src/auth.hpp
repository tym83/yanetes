#pragma once

#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <userver/components/component_base.hpp>
#include <userver/engine/mutex.hpp>
#include <userver/formats/json/value.hpp>

namespace yapi {

class Store;

struct User {
    std::string name;
    std::string uid;
    std::vector<std::string> groups;
    std::map<std::string, std::vector<std::string>> extra;

    userver::formats::json::Value ToJson() const;
};

// Who is asking: static tokens in the format of kube-apiserver's
// --token-auth-file, and service account tokens this server signs itself.
class Auth final : public userver::components::ComponentBase {
public:
    static constexpr std::string_view kName = "yapi-auth";

    Auth(const userver::components::ComponentConfig& config, const userver::components::ComponentContext& context);
    ~Auth() override;

    static userver::yaml_config::Schema GetStaticConfigSchema();

    // The user behind an Authorization header value, if any.
    std::optional<User> Authenticate(std::string_view header);
    // The user behind a bare token, and the audiences it was accepted for.
    std::optional<User> Review(const std::string& token, std::vector<std::string>& audiences);

    struct Issued {
        std::string token;
        std::chrono::system_clock::time_point expires;
    };
    // A token for the service account, as TokenRequest asks for one.
    // Throws ApiError when the account or the bound object is not there.
    Issued Issue(
        const userver::formats::json::Value& service_account,
        const std::vector<std::string>& audiences,
        std::chrono::seconds ttl,
        const std::optional<userver::formats::json::Value>& bound_pod,
        const std::optional<userver::formats::json::Value>& bound_secret,
        const std::optional<userver::formats::json::Value>& node
    ) const;

    const std::vector<std::string>& ApiAudiences() const { return audiences_; }
    // /.well-known/openid-configuration and /openid/v1/jwks.
    std::string OpenIdConfiguration() const;
    std::string Jwks() const;

private:
    std::optional<User> Verify(const std::string& token, std::vector<std::string>& audiences);

    Store& store_;
    std::unordered_map<std::string, User> static_;
    std::string issuer_;
    std::string jwks_uri_;
    std::vector<std::string> audiences_;
    struct Keys;
    std::unique_ptr<Keys> keys_;

    // Checked tokens, for a few seconds: a service account deleted now is
    // locked out within that time, not on the next request.
    struct Cached {
        std::optional<User> user;
        std::vector<std::string> audiences;
        std::chrono::steady_clock::time_point until;
    };
    userver::engine::Mutex cache_mutex_;
    std::unordered_map<std::string, Cached> cache_;
};

}  // namespace yapi
