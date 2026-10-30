#pragma once

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
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
    // reads them from the log itself.
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

// Fans the event log of each resource out to its watchers. A write kicks the
// resource's reader, which takes everything new from the log in one query:
// under load the events of many writes arrive in one batch.
class Watch final : public userver::components::ComponentBase {
public:
    static constexpr std::string_view kName = "yapi-watch";

    Watch(const userver::components::ComponentConfig& config, const userver::components::ComponentContext& context);
    ~Watch() override;

    std::shared_ptr<Subscription> Subscribe(const Resource& r, std::uint64_t after);
    void Kick(const Resource& r);

    // The newest revision handed to watchers.
    std::uint64_t Delivered(const Resource& r);

private:
    struct Channel {
        const Resource* resource = nullptr;
        userver::engine::Mutex mutex;
        std::uint64_t delivered = 0;
        std::vector<std::weak_ptr<Subscription>> subs;
        userver::engine::SingleConsumerEvent kick;
    };

    Channel& ChannelFor(const Resource& r);
    void Run(Channel& ch);

    Store& store_;
    userver::engine::Mutex channels_mutex_;
    std::unordered_map<std::string, std::unique_ptr<Channel>> channels_;
    userver::concurrent::BackgroundTaskStorage tasks_;
};

}  // namespace yapi
