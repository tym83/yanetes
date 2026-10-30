#include <watch.hpp>

#include <userver/components/component_context.hpp>
#include <userver/engine/sleep.hpp>
#include <userver/engine/task/cancel.hpp>
#include <userver/logging/log.hpp>

namespace yapi {

namespace engine = userver::engine;

namespace {

// A watcher this far behind is dropped; it lists again and resumes.
constexpr std::size_t kMaxQueued = 100'000;
constexpr std::size_t kBatch = 5'000;
// Writes from other replicas do not kick this one: the reader looks anyway.
constexpr auto kPoll = std::chrono::milliseconds(500);

}  // namespace

std::optional<WatchEvent> Subscription::Next(std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    cv_.WaitFor(lock, timeout, [&] { return !queue_.empty() || dropped_; });
    if (queue_.empty()) return std::nullopt;
    auto e = std::move(queue_.front());
    queue_.pop_front();
    return e;
}

void Subscription::Push(const WatchEvent& e) {
    {
        std::unique_lock lock(mutex_);
        if (dropped_ || e.rv <= after_ || e.rv <= replay_until_) return;
        if (queue_.size() >= kMaxQueued) {
            dropped_ = true;
            queue_.clear();
        } else {
            queue_.push_back(e);
        }
    }
    cv_.NotifyAll();
}

Watch::Watch(const userver::components::ComponentConfig& config, const userver::components::ComponentContext& context)
    : ComponentBase(config, context), store_(context.FindComponent<Store>()) {
    store_.SetWatch(this);
}

Watch::~Watch() {
    store_.SetWatch(nullptr);
    tasks_.CancelAndWait();
}

Watch::Channel& Watch::ChannelFor(const Resource& r) {
    std::unique_lock lock(channels_mutex_);
    auto& slot = channels_[r.Key()];
    if (!slot) {
        slot = std::make_unique<Channel>();
        slot->resource = &r;
        slot->delivered = store_.Revision(r);
        auto* ch = slot.get();
        tasks_.AsyncDetach("watch-" + r.Key(), [this, ch] { Run(*ch); });
    }
    return *slot;
}

std::shared_ptr<Subscription> Watch::Subscribe(const Resource& r, std::uint64_t after) {
    auto& ch = ChannelFor(r);
    auto sub = std::make_shared<Subscription>(after);
    std::unique_lock lock(ch.mutex);
    sub->replay_until_ = ch.delivered;
    ch.subs.push_back(sub);
    return sub;
}

void Watch::Kick(const Resource& r) { ChannelFor(r).kick.Send(); }

std::uint64_t Watch::Delivered(const Resource& r) {
    auto& ch = ChannelFor(r);
    std::unique_lock lock(ch.mutex);
    return ch.delivered;
}

void Watch::Run(Channel& ch) {
    while (!engine::current_task::ShouldCancel()) {
        (void)ch.kick.WaitForEventFor(kPoll);
        try {
            for (;;) {
                std::uint64_t from;
                {
                    std::unique_lock lock(ch.mutex);
                    from = ch.delivered;
                }
                auto events = store_.EventsAfter(*ch.resource, from, kBatch);
                if (events.empty()) break;
                std::unique_lock lock(ch.mutex);
                std::erase_if(ch.subs, [](const auto& w) { return w.expired(); });
                for (const auto& e : events) {
                    for (const auto& w : ch.subs) {
                        if (auto s = w.lock()) s->Push(e);
                    }
                }
                ch.delivered = events.back().rv;
                if (events.size() < kBatch) break;
            }
        } catch (const std::exception& ex) {
            if (engine::current_task::ShouldCancel()) return;
            LOG_WARNING() << "watch reader of " << ch.resource->Key() << ": " << ex.what();
            engine::InterruptibleSleepFor(std::chrono::seconds(1));
        }
    }
}

}  // namespace yapi
