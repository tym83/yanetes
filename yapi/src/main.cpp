#include <userver/components/component_base.hpp>
#include <userver/components/component_context.hpp>
#include <userver/clients/dns/component.hpp>
#include <userver/clients/http/component_list.hpp>
#include <userver/components/minimal_server_component_list.hpp>
#include <userver/storages/secdist/component.hpp>
#include <userver/storages/secdist/provider_component.hpp>
#include <userver/formats/json/value_builder.hpp>
#include <userver/logging/log.hpp>
#include <userver/utils/daemon_run.hpp>
#include <userver/ydb/component.hpp>
#include <userver/yaml_config/merge_schemas.hpp>

#include <aggregator.hpp>
#include <api.hpp>
#include <auth.hpp>
#include <crds.hpp>
#include <policies.hpp>
#include <webhooks.hpp>
#include <objects.hpp>
#include <proto.hpp>
#include <registry.hpp>
#include <services.hpp>
#include <store.hpp>
#include <watch.hpp>

namespace yapi {

// The namespaces every cluster starts with.
class Bootstrap final : public userver::components::ComponentBase {
public:
    static constexpr std::string_view kName = "yapi-bootstrap";

    Bootstrap(const userver::components::ComponentConfig& config, const userver::components::ComponentContext& context)
        : ComponentBase(config, context) {
        auto& store = context.FindComponent<Store>();
        const auto* ns = Registry::Builtin().Find("", "v1", "namespaces");
        for (const char* name : {"default", "kube-system", "kube-public", "kube-node-lease"}) {
            userver::formats::json::ValueBuilder b(userver::formats::json::Type::kObject);
            b["metadata"]["name"] = name;
            auto object = objects::PrepareCreate(*ns, b.ExtractValue(), "");
            store.Write(*ns, "", name, [&](const auto& current) {
                return current ? Decision::Keep() : Decision::Put(object);
            });
        }
        // The classes upstream creates for the cluster's own pods.
        const auto* pcs = Registry::Builtin().Find("scheduling.k8s.io", "v1", "priorityclasses");
        for (const auto& [name, value] : std::vector<std::pair<std::string, std::int64_t>>{
                 {"system-node-critical", 2000001000}, {"system-cluster-critical", 2000000000}}) {
            userver::formats::json::ValueBuilder b(userver::formats::json::Type::kObject);
            b["metadata"]["name"] = name;
            b["value"] = value;
            b["description"] = "Used for system critical pods that must not be moved from their current node.";
            b["preemptionPolicy"] = "PreemptLowerPriority";
            auto object = objects::PrepareCreate(*pcs, b.ExtractValue(), "");
            store.Write(*pcs, "", name, [&](const auto& current) {
                return current ? Decision::Keep() : Decision::Put(object);
            });
        }
        LOG_INFO() << "yapi: bootstrap namespaces and priority classes in place";
    }
};

// The protobuf schemas clients send typed objects in.
class Protobuf final : public userver::components::ComponentBase {
public:
    static constexpr std::string_view kName = "yapi-protobuf";

    Protobuf(const userver::components::ComponentConfig& config, const userver::components::ComponentContext& context)
        : ComponentBase(config, context), decoder_(config["proto-dir"].As<std::string>()) {
        proto::Decoder::SetInstance(&decoder_);
    }
    ~Protobuf() override { proto::Decoder::SetInstance(nullptr); }

    static userver::yaml_config::Schema GetStaticConfigSchema() {
        return userver::yaml_config::MergeSchemas<ComponentBase>(R"(
type: object
description: protobuf schemas of the Kubernetes types
additionalProperties: false
properties:
    proto-dir:
        type: string
        description: directory holding k8s.io/api, k8s.io/apimachinery and k8s.io/apiextensions-apiserver protos
)");
    }

private:
    proto::Decoder decoder_;
};

}  // namespace yapi

int main(int argc, char* argv[]) {
    auto list = userver::components::MinimalServerComponentList()
                    .Append<userver::components::Secdist>()
                    .Append<userver::components::DefaultSecdistProvider>()
                    .Append<userver::ydb::YdbComponent>()
                    .Append<yapi::Store>()
                    .Append<yapi::Watch>()
                    .Append<yapi::Bootstrap>()
                    .Append<yapi::Auth>()
                    .Append<yapi::Services>()
                    .Append<yapi::Crds>()
                    .Append<userver::clients::dns::Component>()
                    .AppendComponentList(userver::clients::http::ComponentList())
                    .Append<yapi::Webhooks>()
                    .Append<yapi::Aggregator>()
                    .Append<yapi::Policies>()
                    .Append<yapi::Protobuf>();
    // userver matches whole path segments, so the API is registered once per depth.
    for (const char* name : {"api-0", "api-1", "api-2", "api-3", "api-4", "api-5", "api-6", "api-7", "api-8",
                             "apis-0", "apis-1", "apis-2", "apis-3", "apis-4", "apis-5", "apis-6", "apis-7", "apis-8",
                             "api-9", "api-10", "api-11", "api-12", "apis-9", "apis-10", "apis-11", "apis-12",
                             "server-1", "openapi-2", "openapi-3", "openapi-4", "openapi-5",
                             "well-known-2", "openid-3"}) {
        list.Append<yapi::Api>(name);
    }
    return userver::utils::DaemonMain(argc, argv, list);
}
