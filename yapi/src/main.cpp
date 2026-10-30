#include <userver/components/component_base.hpp>
#include <userver/components/component_context.hpp>
#include <userver/components/minimal_server_component_list.hpp>
#include <userver/formats/json/value_builder.hpp>
#include <userver/logging/log.hpp>
#include <userver/utils/daemon_run.hpp>
#include <userver/ydb/component.hpp>

#include <api.hpp>
#include <objects.hpp>
#include <registry.hpp>
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
        LOG_INFO() << "yapi: bootstrap namespaces in place";
    }
};

}  // namespace yapi

int main(int argc, char* argv[]) {
    auto list = userver::components::MinimalServerComponentList()
                    .Append<userver::ydb::YdbComponent>()
                    .Append<yapi::Store>()
                    .Append<yapi::Watch>()
                    .Append<yapi::Bootstrap>();
    // userver matches whole path segments, so the API is registered once per depth.
    for (const char* name : {"api-0", "api-1", "api-2", "api-3", "api-4", "api-5", "api-6", "api-7", "api-8",
                             "apis-0", "apis-1", "apis-2", "apis-3", "apis-4", "apis-5", "apis-6", "apis-7", "apis-8",
                             "server-1", "openapi-2", "openapi-3", "openapi-4", "openapi-5"}) {
        list.Append<yapi::Api>(name);
    }
    return userver::utils::DaemonMain(argc, argv, list);
}
