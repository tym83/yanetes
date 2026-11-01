#pragma once

#include <optional>
#include <string>

#include <userver/formats/json/value.hpp>

#include <registry.hpp>

namespace yapi::openapi {

// The OpenAPI documents the server publishes: upstream's own for the
// built-in types, read once, with the custom resources' schemas added as
// their definitions come and go.
class Documents {
public:
    explicit Documents(std::string dir);

    // /openapi/v2: the swagger document.
    std::string V2(const Registry& registry) const;
    // /openapi/v3: the index of group versions with hashes clients cache by.
    std::string V3Index(const Registry& registry) const;
    // /openapi/v3/<path>, as "apis/apps/v1"; nullopt when there is none.
    std::optional<std::string> V3(const Registry& registry, const std::string& path) const;

private:
    std::string dir_;
    userver::formats::json::Value v2_;
    userver::formats::json::Value v3_index_;
};

// The definition name upstream gives a custom kind: the group reversed,
// the version, the kind ("com.example.stable.v1.CronTab").
std::string DefinitionName(const Resource& r);

}  // namespace yapi::openapi
