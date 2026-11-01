#pragma once

#include <map>
#include <string>
#include <vector>

#include <userver/components/component_base.hpp>
#include <userver/concurrent/background_task_storage.hpp>
#include <userver/formats/json/value.hpp>
#include <userver/rcu/rcu.hpp>

#include <auth.hpp>

namespace yapi {

class Store;
class Watch;

// What a request asks to do, as upstream's authorizer sees it.
struct Attributes {
    std::string verb;
    bool resource_request = true;
    std::string group, resource, subresource, ns, name;  // resource requests
    std::string path;                                    // other requests
};

// Role-based access control as upstream's RBAC authorizer does it: roles
// and bindings read from the store, ClusterRoles aggregated by label, and
// upstream's default roles and bindings put in place at start. Members of
// system:masters may do everything; nodes get the system:node role.
class Rbac final : public userver::components::ComponentBase {
public:
    static constexpr std::string_view kName = "yapi-rbac";

    Rbac(const userver::components::ComponentConfig& config, const userver::components::ComponentContext& context);
    ~Rbac() override;

    static userver::yaml_config::Schema GetStaticConfigSchema();

    struct Verdict {
        bool allowed = false;
        std::string reason;
    };
    Verdict Authorize(const User& user, const Attributes& attrs) const;

    // The rules a user has in a namespace, for SelfSubjectRulesReview.
    struct Rules {
        std::vector<userver::formats::json::Value> resource, non_resource;
    };
    Rules RulesFor(const User& user, const std::string& ns) const;

    // Rereads roles and bindings now, after this server wrote one.
    void Refresh();

private:
    struct Binding {
        std::string kind, name, ns;  // the binding itself; ns empty for a ClusterRoleBinding
        std::string role_kind, role_name;
        std::string subject;  // as it matched: User "x", Group "y", ServiceAccount "ns:z"
    };
    struct Snapshot {
        std::map<std::string, std::vector<userver::formats::json::Value>> cluster_roles;  // name -> rules
        std::map<std::string, std::vector<userver::formats::json::Value>> roles;          // ns/name -> rules
        std::map<std::string, std::vector<Binding>> by_subject;                         // "u:name" or "g:name"
    };

    void Bootstrap(const std::string& dir);
    void Run();
    std::vector<const Binding*> BindingsOf(const Snapshot& snap, const User& user) const;
    const std::vector<userver::formats::json::Value>* RulesOf(const Snapshot& snap, const Binding& b) const;

    Store& store_;
    Watch& watch_;
    userver::rcu::Variable<Snapshot> snapshot_;
    userver::concurrent::BackgroundTaskStorage tasks_;
};

}  // namespace yapi
