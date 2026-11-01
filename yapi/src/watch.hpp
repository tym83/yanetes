#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <userver/components/component_base.hpp>
#include <userver/concurrent/background_task_storage.hpp>
#include <userver/engine/condition_variable.hpp>
#include <userver/engine/mutex.hpp>
#include <userver/engine/single_consumer_event.hpp>

#include <registry.hpp>
#include <store.hpp>

namespace yapi {

// One watcher's events, in revision order.
class Subscription {
public:
    explicit Subscription(std::uint64_t after) : after_(after) {}

    // Waits for the next event; nullopt on timeout or when the subscription
    // was dropped for falling behind.
    std::optional<WatchEvent> Next(std::chrono::milliseconds timeout);
    bool Dropped() const { return dropped_; }

    // Events up to and including this revision are not queued: the watcher
    // reads them from the log itself. Zero when the cache had them all.
    std::uint64_t ReplayUntil() const { return replay_until_; }

private:
    friend class Watch;
    void Push(const WatchEvent& e);

    std::uint64_t after_;
    std::uint64_t replay_until_ = 0;
    userver::engine::Mutex mutex_;
    userver::engine::ConditionVariable cv_;
    std::deque<WatchEvent> queue_;
    bool dropped_ = false;
};

// The watch cache: for each resource, every object as it is now and the
// recent changes, kept from the event log as upstream's cacher keeps them
// from etcd. A write kicks the resource's reader, which takes everything new
// from the log in one query and fans it out; each change carries the object
// as it was before, so watches with selectors know what left them. Lists
// that may be served from memory are.
class Watch final : public userver::components::ComponentBase {
public:
    static constexpr std::string_view kName = "yapi-watch";

    Watch(const userver::components::ComponentConfig& config, const userver::components::ComponentContext& context);
    ~Watch() override;

    // Changes after a revision. Those still in the cache's window come with
    // the subscription; older ones the watcher replays from the log.
    std::shared_ptr<Subscription> Subscribe(const Resource& r, std::uint64_t after);

    // Every object now and the changes from then on, from one moment.
    struct Snapshot {
        std::vector<userver::formats::json::Value> items;
        std::uint64_t rv = 0;
        std::shared_ptr<Subscription> sub;
    };
    std::optional<Snapshot> SubscribeWithSnapshot(const Resource& r, const std::string& ns);

    void Kick(const Resource& r);

    // The newest revision handed to watchers.
    std::uint64_t Delivered(const Resource& r);

    // A list from memory, once the cache has caught up with the store:
    // what Store::List would answer, without reading YDB.
    std::optional<Store::Page> List(
        const Resource& r,
        const std::string& ns,
        std::size_t limit,
        const std::string& after_ns,
        const std::string& after_name
    );
    // What the cache holds once it has reached a revision this process
    // wrote, without asking the store: as fresh as admission needs, and
    // fresher than the informers upstream's admission reads.
    std::vector<userver::formats::json::Value> Objects(const Resource& r, const std::string& ns, std::uint64_t at_least);
    std::optional<userver::formats::json::Value> Object(const Resource& r, const std::string& ns, const std::string& name, std::uint64_t at_least);

    // How many objects come after a key, for remainingItemCount.
    std::optional<std::uint64_t> Count(const Resource& r, const std::string& ns, const std::string& after_ns, const std::string& after_name);

private:
    using Key = std::pair<std::string, std::string>;  // namespace, name
    struct Channel {
        const Resource* resource = nullptr;
        userver::engine::Mutex mutex;
        userver::engine::ConditionVariable advanced;
        bool ready = false;            // the snapshot is loaded
        std::uint64_t delivered = 0;   // the revision objects are at
        std::uint64_t window_from = 0; // every change after it is in window
        std::map<Key, userver::formats::json::Value> objects;
        std::deque<WatchEvent> window;
        std::vector<std::weak_ptr<Subscription>> subs;
        userver::engine::SingleConsumerEvent kick;
    };

    Channel& ChannelFor(const Resource& r);
    // Waits until the cache holds the store's newest revision.
    bool CaughtUp(Channel& ch);
    // Waits, briefly, until it holds a given one.
    void Reach(Channel& ch, std::uint64_t rv);
    void Run(Channel& ch);
    void Load(Channel& ch);

    Store& store_;
    userver::engine::Mutex channels_mutex_;
    std::unordered_map<std::string, std::unique_ptr<Channel>> channels_;
    userver::concurrent::BackgroundTaskStorage tasks_;
};

}  // namespace yapi
