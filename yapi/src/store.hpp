#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <userver/components/component_base.hpp>
#include <userver/concurrent/background_task_storage.hpp>
#include <userver/engine/mutex.hpp>
#include <userver/engine/single_consumer_event.hpp>
#include <userver/formats/json/value.hpp>
#include <userver/ydb/fwd.hpp>

#include <registry.hpp>


namespace yapi {

class Watch;

// What a write does with the object it found.
struct Decision {
    enum class Action { kKeep, kPut, kRemove };
    Action action = Action::kKeep;
    userver::formats::json::Value object;  // kPut: the new object; kRemove: the object as it goes

    static Decision Keep() { return {}; }
    static Decision Put(userver::formats::json::Value v) { return {Action::kPut, std::move(v)}; }
    static Decision Remove(userver::formats::json::Value v) { return {Action::kRemove, std::move(v)}; }
};

struct WatchEvent {
    std::string type;  // ADDED, MODIFIED, DELETED
    std::uint64_t rv = 0;
    std::string ns;
    userver::formats::json::Value object;
};

// Objects live in YDB, one revision counter per resource: Kubernetes only
// compares resourceVersions within a resource, so writes to pods and to
// leases never queue behind each other the way they do behind etcd's one
// revision. Writes to one resource are committed in batches: each batch is
// one transaction that bumps the counter once, so concurrent writers do not
// fight over its row.
class Store final : public userver::components::ComponentBase {
public:
    static constexpr std::string_view kName = "yapi-store";

    Store(const userver::components::ComponentConfig& config, const userver::components::ComponentContext& context);
    ~Store() override;

    static userver::yaml_config::Schema GetStaticConfigSchema();

    std::optional<userver::formats::json::Value> Get(const Resource& r, const std::string& ns, const std::string& name);

    struct Page {
        std::vector<userver::formats::json::Value> items;
        std::uint64_t rv = 0;
        std::string last_ns, last_name;  // set when the page was cut at the limit
    };
    Page List(
        const Resource& r,
        const std::string& ns,
        std::size_t limit,
        const std::string& after_ns,
        const std::string& after_name
    );

    // Reads the object (if any) and applies the decision in one transaction,
    // possibly together with other writes to the same resource; decide runs
    // again if the transaction is retried, and must not write to the same
    // resource itself. Returns the decision as stored: a kept object comes
    // back as found. What decide throws comes back from Write.
    using Decide = std::function<Decision(const std::optional<userver::formats::json::Value>& current)>;
    Decision Write(const Resource& r, const std::string& ns, const std::string& name, const Decide& decide);

    std::uint64_t Revision(const Resource& r);

    // Events of the resource after the revision, oldest first.
    std::vector<WatchEvent> EventsAfter(const Resource& r, std::uint64_t rv, std::size_t limit);
    // The revision the event log was compacted to: nothing at or before it
    // can be watched from or continued at.
    std::uint64_t Compacted(const Resource& r);
    // Objects of a list after a key, for remainingItemCount.
    std::uint64_t Count(const Resource& r, const std::string& ns, const std::string& after_ns, const std::string& after_name);

    // The oldest revision still in the event log, 0 when it is empty.
    std::uint64_t OldestEvent(const Resource& r);

    void SetWatch(Watch* watch) { watch_ = watch; }

private:
    struct Pending;
    struct Batcher;

    void EnsureSchema();
    Batcher& BatcherFor(const Resource& r);
    void RunBatcher(Batcher& b);
    void Commit(const Resource& r, std::vector<Pending*>& batch);

    std::shared_ptr<userver::ydb::TableClient> table_;
    Watch* watch_ = nullptr;
    userver::engine::Mutex batchers_mutex_;
    std::unordered_map<std::string, std::unique_ptr<Batcher>> batchers_;
    userver::concurrent::BackgroundTaskStorage tasks_;
};

}  // namespace yapi
