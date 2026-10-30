#pragma once

#include <optional>
#include <string>
#include <vector>

namespace yapi {

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

    // The storage key of the resource: its revisions are counted per key.
    std::string Key() const { return group.empty() ? plural : plural + "." + group; }
    std::string GroupVersion() const { return group.empty() ? version : group + "/" + version; }
};

class Registry {
public:
    static const Registry& Builtin();

    const Resource* Find(const std::string& group, const std::string& version, const std::string& plural) const;
    const Resource* FindByKind(const std::string& group, const std::string& kind) const;
    const std::vector<Resource>& All() const { return resources_; }

    // Group versions in the order discovery lists them, the core group first.
    std::vector<std::string> GroupVersions() const;

private:
    std::vector<Resource> resources_;
};

}  // namespace yapi
