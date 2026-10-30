#pragma once

#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <userver/engine/mutex.hpp>
#include <userver/formats/json/value.hpp>
#include <userver/rcu/rcu.hpp>

namespace yapi {

// What a custom resource's definition says about one served version.
struct CustomInfo {
    std::string crd;                // the definition's name, plural.group
    std::string storage_version;    // the version objects are stored in
    userver::formats::json::Value schema;  // openAPIV3Schema of this version, may be missing
    bool status = false;
    bool scale = false;
    std::string spec_replicas, status_replicas, label_selector;  // scale paths
    userver::formats::json::Value printer_columns;
    bool webhook_conversion = false;
    userver::formats::json::Value webhook;  // conversion.webhook of the definition
    userver::formats::json::Value conversion_review_versions;
};

// One kind of object the server stores, as discovery describes it.
struct Resource {
    std::string group;  // "" for the core group
    std::string version;
    std::string plural;
    std::string singular;
    std::string kind;
    bool namespaced = true;
    bool has_status = false;  // a status subresource: spec and status are written apart
    std::vector<std::string> short_names;
    std::vector<std::string> categories;
    // Empty for an ordinary stored resource. Reviews and the like take only
    // "create": the server answers them and keeps nothing.
    std::vector<std::string> verbs;
    // Set for resources a CustomResourceDefinition serves.
    std::shared_ptr<const CustomInfo> custom;

    bool CreateOnly() const { return verbs.size() == 1 && verbs[0] == "create"; }

    // The storage key of the resource: its revisions are counted per key,
    // whatever version a client speaks.
    std::string Key() const { return group.empty() ? plural : plural + "." + group; }
    std::string GroupVersion() const { return group.empty() ? version : group + "/" + version; }
};

// The built-in resources and those custom resource definitions add. A
// resource handed out stays valid for the life of the process: a changed
// definition gets new entries, the old ones are only no longer listed.
class Registry {
public:
    static const Registry& Builtin();

    const Resource* Find(const std::string& group, const std::string& version, const std::string& plural) const;
    const Resource* FindByKind(const std::string& group, const std::string& kind) const;
    std::vector<const Resource*> All() const;

    // Group versions in the order discovery lists them, the core group first.
    std::vector<std::string> GroupVersions() const;
    // The preferred version of a group: the storage version of a custom one.
    std::string PreferredVersion(const std::string& group) const;

    // Replaces what one definition serves; an empty list stops serving it.
    void SetCustom(const std::string& crd, std::vector<Resource> resources) const;

private:
    explicit Registry(std::vector<Resource> resources) : resources_(std::move(resources)) {}

    std::vector<Resource> resources_;
    mutable userver::engine::Mutex storage_mutex_;
    mutable std::deque<Resource> storage_;
    mutable userver::rcu::Variable<std::map<std::string, std::vector<const Resource*>>> custom_;
};

}  // namespace yapi
