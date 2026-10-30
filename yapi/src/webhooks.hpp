#pragma once

#include <memory>
#include <string>
#include <vector>

#include <userver/clients/http/client.hpp>
#include <userver/components/component_base.hpp>
#include <userver/concurrent/background_task_storage.hpp>
#include <userver/formats/json/value.hpp>
#include <userver/rcu/rcu.hpp>

#include <auth.hpp>
#include <registry.hpp>

namespace yapi {

class Store;
class Watch;

// Admission webhooks and conversion webhooks: the calls kube-apiserver
// makes out to the cluster's own services. The configurations are kept
// in memory, refreshed from their watch.
class Webhooks final : public userver::components::ComponentBase {
public:
    static constexpr std::string_view kName = "yapi-webhooks";

    Webhooks(const userver::components::ComponentConfig& config, const userver::components::ComponentContext& context);
    ~Webhooks() override;

    struct Request {
        const Resource* resource = nullptr;
        std::string subresource;
        std::string operation;  // CREATE, UPDATE, DELETE, CONNECT
        std::string ns, name;
        userver::formats::json::Value object, old_object, options;
        const User* user = nullptr;
        bool dry_run = false;
    };

    // Whether any webhook might want this request: lets writes take the
    // slower path that calls out only when they have to.
    bool Interested(const Resource& r, const std::string& subresource, const std::string& operation) const;

    // Mutating webhooks in order; the object they leave. Throws ApiError.
    userver::formats::json::Value Mutate(const Request& request) const;
    // Validating webhooks; throws ApiError when one refuses.
    void Validate(const Request& request) const;

    // A conversion webhook's answer for objects of a custom resource.
    std::vector<userver::formats::json::Value> Convert(
        const CustomInfo& info,
        const std::vector<userver::formats::json::Value>& objects,
        const std::string& desired_api_version
    ) const;

    // Checks a webhook configuration before it is stored: match conditions
    // must compile. Throws ApiError.
    static void ValidateConfig(const Resource& r, const userver::formats::json::Value& config);

    // Rereads the configurations now, after this server wrote one: a
    // client that just registered a webhook expects it to apply.
    void Refresh();

private:
    struct Hook {
        std::string name;
        userver::formats::json::Value spec;  // one entry of .webhooks
    };
    struct Snapshot {
        std::vector<Hook> mutating, validating;
    };

    void Run();
    bool Matches(const Hook& hook, const Request& request) const;
    userver::formats::json::Value Call(
        const userver::formats::json::Value& client_config,
        const std::string& name,
        const userver::formats::json::Value& body,
        int timeout_seconds
    ) const;
    userver::formats::json::Value Review(const Hook& hook, const Request& request, const userver::formats::json::Value& object) const;

    Store& store_;
    Watch& watch_;
    userver::clients::http::Client& http_;
    userver::rcu::Variable<Snapshot> hooks_;
    userver::concurrent::BackgroundTaskStorage tasks_;
};

}  // namespace yapi
