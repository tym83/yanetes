#pragma once

#include <string>
#include <vector>

#include <userver/components/component_base.hpp>
#include <userver/concurrent/background_task_storage.hpp>
#include <userver/formats/json/value.hpp>
#include <userver/rcu/rcu.hpp>

#include <webhooks.hpp>

namespace yapi {

class Crds;
class Store;

// ValidatingAdmissionPolicy and MutatingAdmissionPolicy: admission in CEL,
// evaluated in the server itself. Policies and bindings are kept in
// memory; a policy's type check lands in its status.
class Policies final : public userver::components::ComponentBase {
public:
    static constexpr std::string_view kName = "yapi-policies";

    Policies(const userver::components::ComponentConfig& config, const userver::components::ComponentContext& context);
    ~Policies() override;

    // Whether any binding might apply: writes take the slower path then.
    bool Interested(const Resource& r, const std::string& operation) const;

    // Mutations of the matching policies, in order. Throws ApiError.
    userver::formats::json::Value Mutate(const Webhooks::Request& request) const;
    // Validations; throws ApiError on a Deny, returns the Warn messages.
    std::vector<std::string> Validate(const Webhooks::Request& request) const;

    // Rereads policies and bindings, and type checks policies that changed.
    void Refresh();

private:
    struct Snapshot {
        std::vector<userver::formats::json::Value> validating, validating_bindings, mutating, mutating_bindings;
    };

    bool Matches(const userver::formats::json::Value& policy, const userver::formats::json::Value& binding,
                 const Webhooks::Request& request) const;
    void TypeCheck(const userver::formats::json::Value& policy);

    Store& store_;
    Crds& crds_;
    userver::rcu::Variable<Snapshot> snapshot_;
    userver::concurrent::BackgroundTaskStorage tasks_;
};

}  // namespace yapi
