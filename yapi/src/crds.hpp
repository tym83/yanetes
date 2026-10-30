#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>

#include <userver/components/component_base.hpp>
#include <userver/concurrent/background_task_storage.hpp>
#include <userver/formats/json/value.hpp>

#include <registry.hpp>
#include <schema.hpp>

namespace yapi {

class Store;
class Watch;

// What apiextensions-apiserver does inside kube-apiserver: serves the
// resources custom resource definitions describe, keeps the definitions'
// status, empties a definition's resources before it goes. Also hands out
// schemas, the definitions' and the built-in types' from upstream OpenAPI.
class Crds final : public userver::components::ComponentBase {
public:
    static constexpr std::string_view kName = "yapi-crds";

    Crds(const userver::components::ComponentConfig& config, const userver::components::ComponentContext& context);
    ~Crds() override;

    static userver::yaml_config::Schema GetStaticConfigSchema();

    // The schema an object of the resource is checked against: a custom
    // resource's structural schema, or a built-in type's OpenAPI one.
    std::optional<schema::Schema> SchemaFor(const Resource& r) const;

    // Checks a definition before it is stored; throws ApiError.
    static void Validate(const userver::formats::json::Value& crd);

    // Converts an object of a custom resource to a version of it.
    userver::formats::json::Value Convert(const Resource& r, const userver::formats::json::Value& object, const std::string& version) const;

private:
    void Register(const userver::formats::json::Value& crd);
    void Reconcile(const userver::formats::json::Value& crd);
    void Run();

    Store& store_;
    Watch& watch_;
    std::map<std::string, userver::formats::json::Value> components_;
    std::map<std::string, std::string> typed_;  // group/version/kind -> component name
    userver::concurrent::BackgroundTaskStorage tasks_;
};

}  // namespace yapi
