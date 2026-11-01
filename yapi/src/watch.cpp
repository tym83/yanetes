#include <watch.hpp>

#include <userver/components/component_context.hpp>
#include <userver/engine/sleep.hpp>
#include <userver/engine/task/cancel.hpp>
#include <userver/logging/log.hpp>

#include <objects.hpp>

namespace yapi {

namespace engine = userver::engine;

namespace {

// A watcher this far behind is dropped; it lists again and resumes.
constexpr std::size_t kMaxQueued = 100'000;
constexpr std::size_t kBatch = 5'000;
// The changes kept for watches that start from a recent revision.
constexpr std::size_t kWindow = 100'000;
// Writes from other replicas do not kick this one: the reader looks anyway.
constexpr auto kPoll = std::chrono::milliseconds(500);
// How long a list waits for the cache to catch up before it reads YDB.
constexpr auto kCatchUp = std::chrono::seconds(3);
constexpr auto kLoad = std::chrono::seconds(60);

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
    Channel* ch = nullptr;
    {
        std::unique_lock lock(channels_mutex_);
        auto& slot = channels_[r.Key()];
        if (!slot) {
            slot = std::make_unique<Channel>();
            slot->resource = &r;
            auto* created = slot.get();
            tasks_.AsyncDetach("watch-" + r.Key(), [this, created] { Run(*created); });
        }
        ch = slot.get();
    }
    // Everyone but the reader waits for the snapshot.
    std::unique_lock lock(ch->mutex);
    ch->advanced.WaitFor(lock, kLoad, [&] { return ch->ready; });
    if (!ch->ready) throw std::runtime_error("the watch cache of " + r.Key() + " is not loaded yet");
    return *ch;
}

std::shared_ptr<Subscription> Watch::Subscribe(const Resource& r, std::uint64_t after) {
    auto& ch = ChannelFor(r);
    auto sub = std::make_shared<Subscription>(after);
    std::unique_lock lock(ch.mutex);
    if (after >= ch.window_from) {
        // The cache has every change since: they come with the subscription.
        for (const auto& e : ch.window) {
            if (e.rv > after) sub->Push(e);
        }
    } else {
        sub->replay_until_ = ch.delivered;
    }
    ch.subs.push_back(sub);
    return sub;
}

std::optional<Watch::Snapshot> Watch::SubscribeWithSnapshot(const Resource& r, const std::string& ns) {
    auto& ch = ChannelFor(r);
    Snapshot snap;
    std::unique_lock lock(ch.mutex);
    for (const auto& [key, object] : ch.objects) {
        if (ns.empty() || key.first == ns) snap.items.push_back(object);
    }
    snap.rv = ch.delivered;
    snap.sub = std::make_shared<Subscription>(ch.delivered);
    ch.subs.push_back(snap.sub);
    return snap;
}

void Watch::Kick(const Resource& r) {
    std::unique_lock lock(channels_mutex_);
    auto it = channels_.find(r.Key());
    if (it != channels_.end()) it->second->kick.Send();
}

std::uint64_t Watch::Delivered(const Resource& r) {
    auto& ch = ChannelFor(r);
    std::unique_lock lock(ch.mutex);
    return ch.delivered;
}

bool Watch::CaughtUp(Channel& ch) {
    const auto want = store_.Revision(*ch.resource);
    ch.kick.Send();
    std::unique_lock lock(ch.mutex);
    return ch.advanced.WaitFor(lock, kCatchUp, [&] { return ch.delivered >= want; });
}

std::optional<Store::Page> Watch::List(
    const Resource& r,
    const std::string& ns,
    std::size_t limit,
    const std::string& after_ns,
    const std::string& after_name
) {
    auto& ch = ChannelFor(r);
    if (!CaughtUp(ch)) return std::nullopt;
    std::unique_lock lock(ch.mutex);
    Store::Page page;
    page.rv = ch.delivered;
    auto it = ns.empty() ? ch.objects.upper_bound({after_ns, after_name}) : ch.objects.upper_bound({ns, after_name});
    for (; it != ch.objects.end(); ++it) {
        if (!ns.empty() && it->first.first != ns) break;
        if (limit != 0 && page.items.size() == limit) {
            page.last_ns = std::prev(it)->first.first;
            page.last_name = std::prev(it)->first.second;
            break;
        }
        page.items.push_back(it->second);
    }
    return page;
}

void Watch::Reach(Channel& ch, std::uint64_t rv) {
    {
        std::unique_lock lock(ch.mutex);
        if (ch.delivered >= rv) return;
    }
    ch.kick.Send();
    std::unique_lock lock(ch.mutex);
    ch.advanced.WaitFor(lock, kCatchUp, [&] { return ch.delivered >= rv; });
}

std::vector<userver::formats::json::Value> Watch::Objects(const Resource& r, const std::string& ns, std::uint64_t at_least) {
    auto& ch = ChannelFor(r);
    Reach(ch, at_least);
    std::unique_lock lock(ch.mutex);
    std::vector<userver::formats::json::Value> out;
    auto it = ns.empty() ? ch.objects.begin() : ch.objects.lower_bound({ns, ""});
    for (; it != ch.objects.end() && (ns.empty() || it->first.first == ns); ++it) out.push_back(it->second);
    return out;
}

std::optional<userver::formats::json::Value> Watch::Object(const Resource& r, const std::string& ns, const std::string& name, std::uint64_t at_least) {
    auto& ch = ChannelFor(r);
    Reach(ch, at_least);
    std::unique_lock lock(ch.mutex);
    auto it = ch.objects.find({ns, name});
    if (it == ch.objects.end()) return std::nullopt;
    return it->second;
}

std::optional<std::uint64_t> Watch::Count(const Resource& r, const std::string& ns, const std::string& after_ns, const std::string& after_name) {
    auto& ch = ChannelFor(r);
    std::unique_lock lock(ch.mutex);
    std::uint64_t n = 0;
    auto it = ns.empty() ? ch.objects.upper_bound({after_ns, after_name}) : ch.objects.upper_bound({ns, after_name});
    for (; it != ch.objects.end() && (ns.empty() || it->first.first == ns); ++it) ++n;
    return n;
}

void Watch::Load(Channel& ch) {
    auto page = store_.List(*ch.resource, "", 0, "", "");
    std::unique_lock lock(ch.mutex);
    for (auto& item : page.items) {
        Key key{objects::Namespace(item), objects::Name(item)};
        ch.objects.emplace(std::move(key), std::move(item));
    }
    ch.delivered = page.rv;
    ch.window_from = page.rv;
    ch.ready = true;
    ch.advanced.NotifyAll();
}

void Watch::Run(Channel& ch) {
    while (!engine::current_task::ShouldCancel()) {
        try {
            Load(ch);
            break;
        } catch (const std::exception& ex) {
            if (engine::current_task::ShouldCancel()) return;
            LOG_WARNING() << "watch cache of " << ch.resource->Key() << " not loaded: " << ex.what();
            engine::InterruptibleSleepFor(std::chrono::seconds(1));
        }
    }
    while (!engine::current_task::ShouldCancel()) {
        (void)ch.kick.WaitForEventFor(kPoll);
        try {
            for (;;) {
                // Every change up to the store's revision is in the log once
                // the revision is: read it first, then the changes.
                const auto newest = store_.Revision(*ch.resource);
                std::uint64_t from;
                {
                    std::unique_lock lock(ch.mutex);
                    from = ch.delivered;
                }
                auto events = store_.EventsAfter(*ch.resource, from, kBatch);
                std::unique_lock lock(ch.mutex);
                std::erase_if(ch.subs, [](const auto& w) { return w.expired(); });
                for (auto& e : events) {
                    Key key{e.ns, objects::Name(e.object)};
                    auto it = ch.objects.find(key);
                    e.prev_known = true;
                    if (it != ch.objects.end()) e.prev = it->second;
                    if (e.type == "DELETED") {
                        if (it != ch.objects.end()) ch.objects.erase(it);
                    } else if (it != ch.objects.end()) {
                        it->second = e.object;
                    } else {
                        ch.objects.emplace(std::move(key), e.object);
                    }
                    for (const auto& w : ch.subs) {
                        if (auto s = w.lock()) s->Push(e);
                    }
                    ch.window.push_back(std::move(e));
                    if (ch.window.size() > kWindow) {
                        ch.window_from = ch.window.front().rv;
                        ch.window.pop_front();
                    }
                }
                if (!events.empty()) ch.delivered = ch.window.back().rv;
                // A revision with no change behind it (compaction moves an
                // idle resource on) is reached all the same.
                if (events.size() < kBatch && newest > ch.delivered) ch.delivered = newest;
                ch.advanced.NotifyAll();
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
