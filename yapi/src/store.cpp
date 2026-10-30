#include <store.hpp>

#include <deque>
#include <map>

#include <userver/components/component_config.hpp>
#include <userver/components/component_context.hpp>
#include <userver/engine/future.hpp>
#include <userver/engine/sleep.hpp>
#include <userver/engine/task/cancel.hpp>
#include <userver/formats/json/serialize.hpp>
#include <userver/formats/json/value_builder.hpp>
#include <userver/formats/serialize/common_containers.hpp>
#include <userver/logging/log.hpp>
#include <userver/ydb/component.hpp>
#include <userver/ydb/io/structs.hpp>
#include <userver/ydb/table.hpp>
#include <userver/ydb/transaction.hpp>
#include <userver/yaml_config/merge_schemas.hpp>

#include <watch.hpp>

namespace yapi {

// Rows passed to YDB as lists of structs; the member names are the column
// names. They may not live in an anonymous namespace.
namespace rows {

struct Key {
    userver::ydb::Utf8 res;
    userver::ydb::Utf8 ns;
    userver::ydb::Utf8 name;
    static constexpr userver::ydb::StructMemberNames kYdbMemberNames{};
};

struct Object {
    userver::ydb::Utf8 res;
    userver::ydb::Utf8 ns;
    userver::ydb::Utf8 name;
    std::uint64_t rv;
    std::string value;
    static constexpr userver::ydb::StructMemberNames kYdbMemberNames{};
};

struct Event {
    userver::ydb::Utf8 res;
    std::uint64_t rv;
    userver::ydb::Utf8 type;
    userver::ydb::Utf8 ns;
    std::string value;
    static constexpr userver::ydb::StructMemberNames kYdbMemberNames{};
};

}  // namespace rows

namespace json = userver::formats::json;
namespace ydb = userver::ydb;
using userver::storages::Query;

namespace {

// How often the event log is compacted, and so how old a watch or a
// continue token can get: one to two rounds.
constexpr auto kCompactEvery = std::chrono::seconds(150);

// `resource` is a keyword in YQL, so the column is `res`.
const Query kSchema{R"(
CREATE TABLE IF NOT EXISTS objects (
    res Utf8, ns Utf8, name Utf8, rv Uint64, value String,
    PRIMARY KEY (res, ns, name)
);
CREATE TABLE IF NOT EXISTS revisions (
    res Utf8, rv Uint64,
    PRIMARY KEY (res)
);
CREATE TABLE IF NOT EXISTS events (
    res Utf8, rv Uint64, type Utf8, ns Utf8, value String,
    PRIMARY KEY (res, rv)
);
CREATE TABLE IF NOT EXISTS compactions (
    res Utf8, rv Uint64,
    PRIMARY KEY (res)
);
)"};

const Query kGet{
    R"(
DECLARE $resource AS Utf8; DECLARE $ns AS Utf8; DECLARE $name AS Utf8;
SELECT value FROM objects WHERE res = $resource AND ns = $ns AND name = $name;
)",
    Query::Name{"get"}};

const Query kBatchRead{
    R"(
DECLARE $resource AS Utf8;
DECLARE $keys AS List<Struct<res:Utf8, ns:Utf8, name:Utf8>>;
SELECT rv FROM revisions WHERE res = $resource;
SELECT o.ns AS ns, o.name AS name, o.value AS value
FROM AS_TABLE($keys) AS k INNER JOIN objects AS o ON o.res = k.res AND o.ns = k.ns AND o.name = k.name;
)",
    Query::Name{"batch-read"}};

const Query kBatchWrite{
    R"(
DECLARE $resource AS Utf8; DECLARE $rv AS Uint64;
DECLARE $puts AS List<Struct<res:Utf8, ns:Utf8, name:Utf8, rv:Uint64, value:String>>;
DECLARE $dels AS List<Struct<res:Utf8, ns:Utf8, name:Utf8>>;
DECLARE $events AS List<Struct<res:Utf8, rv:Uint64, type:Utf8, ns:Utf8, value:String>>;
UPSERT INTO revisions (res, rv) VALUES ($resource, $rv);
UPSERT INTO objects SELECT * FROM AS_TABLE($puts);
DELETE FROM objects ON SELECT * FROM AS_TABLE($dels);
UPSERT INTO events SELECT * FROM AS_TABLE($events);
)",
    Query::Name{"batch-write"}};

const Query kListAll{
    R"(
DECLARE $resource AS Utf8; DECLARE $after_ns AS Utf8; DECLARE $after_name AS Utf8; DECLARE $limit AS Uint64;
SELECT rv FROM revisions WHERE res = $resource;
SELECT ns, name, value FROM objects
WHERE res = $resource AND (ns > $after_ns OR (ns = $after_ns AND name > $after_name))
ORDER BY ns, name LIMIT $limit;
)",
    Query::Name{"list-all"}};

const Query kListNs{
    R"(
DECLARE $resource AS Utf8; DECLARE $ns AS Utf8; DECLARE $after_name AS Utf8; DECLARE $limit AS Uint64;
SELECT rv FROM revisions WHERE res = $resource;
SELECT ns, name, value FROM objects
WHERE res = $resource AND ns = $ns AND name > $after_name
ORDER BY ns, name LIMIT $limit;
)",
    Query::Name{"list-ns"}};

const Query kRevision{
    R"(
DECLARE $resource AS Utf8;
SELECT rv FROM revisions WHERE res = $resource;
)",
    Query::Name{"revision"}};

const Query kEventsAfter{
    R"(
DECLARE $resource AS Utf8; DECLARE $rv AS Uint64; DECLARE $limit AS Uint64;
SELECT rv, type, ns, value FROM events WHERE res = $resource AND rv > $rv ORDER BY rv LIMIT $limit;
)",
    Query::Name{"events-after"}};

const Query kCompacted{
    R"(
DECLARE $resource AS Utf8;
SELECT rv FROM compactions WHERE res = $resource;
)",
    Query::Name{"compacted"}};

const Query kAllRevisions{
    R"(
SELECT res, rv FROM revisions;
)",
    Query::Name{"all-revisions"}};

const Query kCompact{
    R"(
DECLARE $resource AS Utf8; DECLARE $rv AS Uint64;
DELETE FROM events WHERE res = $resource AND rv <= $rv;
UPSERT INTO compactions (res, rv) VALUES ($resource, $rv);
)",
    Query::Name{"compact"}};

const Query kCountAll{
    R"(
DECLARE $resource AS Utf8; DECLARE $after_ns AS Utf8; DECLARE $after_name AS Utf8;
SELECT COUNT(*) AS n FROM objects
WHERE res = $resource AND (ns > $after_ns OR (ns = $after_ns AND name > $after_name));
)",
    Query::Name{"count-all"}};

const Query kCountNs{
    R"(
DECLARE $resource AS Utf8; DECLARE $ns AS Utf8; DECLARE $after_name AS Utf8;
SELECT COUNT(*) AS n FROM objects WHERE res = $resource AND ns = $ns AND name > $after_name;
)",
    Query::Name{"count-ns"}};

const Query kOldestEvent{
    R"(
DECLARE $resource AS Utf8;
SELECT rv FROM events WHERE res = $resource ORDER BY rv LIMIT 1;
)",
    Query::Name{"oldest-event"}};

// The number past which a list's revision row reads as "nothing written yet".
std::uint64_t FirstRv(ydb::Cursor cursor) {
    for (auto row : cursor) return row.Get<std::optional<std::uint64_t>>("rv").value_or(0);
    return 0;
}

json::Value WithRv(const json::Value& object, std::uint64_t rv) {
    json::ValueBuilder b(object);
    b["metadata"]["resourceVersion"] = std::to_string(rv);
    return b.ExtractValue();
}

}  // namespace

Store::Store(const userver::components::ComponentConfig& config, const userver::components::ComponentContext& context)
    : ComponentBase(config, context),
      table_(context.FindComponent<ydb::YdbComponent>().GetTableClient(config["dbname"].As<std::string>())) {
    EnsureSchema();
    // Compaction, as etcd does it: what was the newest revision one round
    // ago goes from the event log now. Watches and continue tokens older
    // than that get 410 and start over.
    tasks_.AsyncDetach("compact", [this] {
        std::map<std::string, std::uint64_t> previous;
        while (!userver::engine::current_task::ShouldCancel()) {
            userver::engine::InterruptibleSleepFor(kCompactEvery);
            if (userver::engine::current_task::ShouldCancel()) return;
            try {
                std::map<std::string, std::uint64_t> now;
                auto res = table_->ExecuteQuery(kAllRevisions);
                for (auto row : res.GetSingleCursor()) {
                    now[row.Get<std::optional<ydb::Utf8>>("res").value_or(ydb::Utf8{}).GetUnderlying()] =
                        row.Get<std::optional<std::uint64_t>>("rv").value_or(0);
                }
                for (const auto& [key, rv] : previous) {
                    if (rv > 0) table_->ExecuteQuery(kCompact, "$resource", ydb::Utf8{key}, "$rv", rv);
                }
                previous = std::move(now);
            } catch (const std::exception& e) {
                LOG_WARNING() << "compaction: " << e.what();
            }
        }
    });
}

Store::~Store() { tasks_.CancelAndWait(); }

userver::yaml_config::Schema Store::GetStaticConfigSchema() {
    return userver::yaml_config::MergeSchemas<ComponentBase>(R"(
type: object
description: object storage in YDB
additionalProperties: false
properties:
    dbname:
        type: string
        description: database key in the ydb component
)");
}

void Store::EnsureSchema() { table_->ExecuteSchemeQuery(std::string(kSchema.GetStatementView())); }

std::optional<json::Value> Store::Get(const Resource& r, const std::string& ns, const std::string& name) {
    auto res = table_->ExecuteQuery(
        kGet, "$resource", ydb::Utf8{r.Key()}, "$ns", ydb::Utf8{ns}, "$name", ydb::Utf8{name}
    );
    for (auto row : res.GetSingleCursor()) {
        return json::FromString(row.Get<std::optional<std::string>>("value").value_or("{}"));
    }
    return std::nullopt;
}

Store::Page Store::List(
    const Resource& r,
    const std::string& ns,
    std::size_t limit,
    const std::string& after_ns,
    const std::string& after_name
) {
    // One more than asked tells whether there is a next page.
    std::uint64_t fetch = limit == 0 ? std::numeric_limits<std::uint32_t>::max() : limit + 1;
    auto res = ns.empty() ? table_->ExecuteQuery(
                                kListAll,
                                "$resource",
                                ydb::Utf8{r.Key()},
                                "$after_ns",
                                ydb::Utf8{after_ns},
                                "$after_name",
                                ydb::Utf8{after_name},
                                "$limit",
                                fetch
                            )
                          : table_->ExecuteQuery(
                                kListNs,
                                "$resource",
                                ydb::Utf8{r.Key()},
                                "$ns",
                                ydb::Utf8{ns},
                                "$after_name",
                                ydb::Utf8{after_name},
                                "$limit",
                                fetch
                            );
    Page page;
    page.rv = FirstRv(res.GetCursor(0));
    bool more = false;
    std::string last_ns, last_name;
    for (auto row : res.GetCursor(1)) {
        auto row_ns = row.Get<std::optional<ydb::Utf8>>("ns").value_or(ydb::Utf8{});
        auto row_name = row.Get<std::optional<ydb::Utf8>>("name").value_or(ydb::Utf8{});
        auto value = row.Get<std::optional<std::string>>("value").value_or("{}");
        if (limit != 0 && page.items.size() == limit) {
            more = true;
            break;
        }
        page.items.push_back(json::FromString(value));
        last_ns = row_ns.GetUnderlying();
        last_name = row_name.GetUnderlying();
    }
    if (more) {
        page.last_ns = last_ns;
        page.last_name = last_name;
    }
    return page;
}

// A write waiting for its batch.
struct Store::Pending {
    std::string ns, name;
    const Decide* decide = nullptr;
    userver::engine::Promise<Decision> promise;
};

struct Store::Batcher {
    const Resource* resource = nullptr;
    userver::engine::Mutex mutex;
    std::deque<Pending*> queue;
    userver::engine::SingleConsumerEvent kick;
};

namespace {
// A batch big enough to spread the cost of a commit, small enough to keep
// a retried transaction cheap.
constexpr std::size_t kMaxBatch = 256;
}  // namespace

Store::Batcher& Store::BatcherFor(const Resource& r) {
    std::unique_lock lock(batchers_mutex_);
    auto& slot = batchers_[r.Key()];
    if (!slot) {
        slot = std::make_unique<Batcher>();
        slot->resource = &r;
        auto* b = slot.get();
        tasks_.AsyncDetach("write-" + r.Key(), [this, b] { RunBatcher(*b); });
    }
    return *slot;
}

Decision Store::Write(const Resource& r, const std::string& ns, const std::string& name, const Decide& decide) {
    Pending p{ns, name, &decide, {}};
    auto result = p.promise.get_future();
    auto& b = BatcherFor(r);
    {
        std::unique_lock lock(b.mutex);
        b.queue.push_back(&p);
    }
    b.kick.Send();
    // The batch holds pointers into this frame (the pending write and the
    // caller's decide): a client hanging up must not end the wait early.
    userver::engine::TaskCancellationBlocker block;
    return result.get();
}

void Store::RunBatcher(Batcher& b) {
    while (!userver::engine::current_task::ShouldCancel()) {
        (void)b.kick.WaitForEvent();
        for (;;) {
            std::vector<Pending*> batch;
            {
                std::unique_lock lock(b.mutex);
                while (!b.queue.empty() && batch.size() < kMaxBatch) {
                    batch.push_back(b.queue.front());
                    b.queue.pop_front();
                }
            }
            if (batch.empty()) break;
            Commit(*b.resource, batch);
        }
    }
}

void Store::Commit(const Resource& r, std::vector<Pending*>& batch) {
    struct Outcome {
        Decision decision;
        std::exception_ptr error;
    };
    std::vector<Outcome> outcomes;
    bool written = false;
    try {
        userver::ydb::RetryTxSettings settings;
        settings.tx_mode = ydb::TransactionMode::kSerializableRW;
        settings.retries = 10;
        table_->RetryTx("write", settings, [&](ydb::TxActor& tx) -> ydb::TxAction {
            outcomes.assign(batch.size(), Outcome{});
            written = false;
            const ydb::Utf8 res{r.Key()};
            std::vector<rows::Key> keys;
            std::map<std::pair<std::string, std::string>, std::optional<json::Value>> view;
            for (auto* p : batch) {
                if (view.emplace(std::make_pair(p->ns, p->name), std::nullopt).second) {
                    keys.push_back({res, ydb::Utf8{p->ns}, ydb::Utf8{p->name}});
                }
            }
            auto read = tx.Execute(kBatchRead, "$resource", res, "$keys", keys);
            std::uint64_t rv = FirstRv(read.GetCursor(0));
            for (auto row : read.GetCursor(1)) {
                auto key = std::make_pair(
                    row.Get<std::optional<ydb::Utf8>>("ns").value_or(ydb::Utf8{}).GetUnderlying(),
                    row.Get<std::optional<ydb::Utf8>>("name").value_or(ydb::Utf8{}).GetUnderlying()
                );
                view[key] = json::FromString(row.Get<std::optional<std::string>>("value").value_or("{}"));
            }
            // Each write sees the ones before it in the batch; only the last
            // state of an object is stored, every step goes to the log.
            std::map<std::pair<std::string, std::string>, std::optional<rows::Object>> last;
            std::vector<rows::Event> events;
            for (std::size_t i = 0; i < batch.size(); ++i) {
                auto key = std::make_pair(batch[i]->ns, batch[i]->name);
                auto& current = view[key];
                Decision d;
                try {
                    d = (*batch[i]->decide)(current);
                } catch (...) {
                    outcomes[i].error = std::current_exception();
                    continue;
                }
                if (d.action == Decision::Action::kKeep) {
                    if (current) d.object = *current;
                    outcomes[i].decision = std::move(d);
                    continue;
                }
                ++rv;
                d.object = WithRv(d.object, rv);
                auto value = json::ToString(d.object);
                const bool put = d.action == Decision::Action::kPut;
                events.push_back({res, rv, ydb::Utf8{put ? (current ? "MODIFIED" : "ADDED") : "DELETED"}, ydb::Utf8{key.first}, value});
                if (put) {
                    last[key] = rows::Object{res, ydb::Utf8{key.first}, ydb::Utf8{key.second}, rv, value};
                    current = d.object;
                } else {
                    last[key] = std::nullopt;
                    current = std::nullopt;
                }
                outcomes[i].decision = std::move(d);
            }
            if (events.empty()) return ydb::TxAction::kCommit;
            std::vector<rows::Object> puts;
            std::vector<rows::Key> dels;
            for (auto& [key, row] : last) {
                if (row) {
                    puts.push_back(std::move(*row));
                } else {
                    dels.push_back({res, ydb::Utf8{key.first}, ydb::Utf8{key.second}});
                }
            }
            tx.Execute(kBatchWrite, "$resource", res, "$rv", rv, "$puts", puts, "$dels", dels, "$events", events);
            written = true;
            return ydb::TxAction::kCommit;
        });
    } catch (...) {
        auto error = std::current_exception();
        for (auto* p : batch) p->promise.set_exception(error);
        return;
    }
    if (written && watch_ != nullptr) watch_->Kick(r);
    for (std::size_t i = 0; i < batch.size(); ++i) {
        if (outcomes[i].error) {
            batch[i]->promise.set_exception(outcomes[i].error);
        } else {
            batch[i]->promise.set_value(std::move(outcomes[i].decision));
        }
    }
}

std::uint64_t Store::Revision(const Resource& r) {
    auto res = table_->ExecuteQuery(kRevision, "$resource", ydb::Utf8{r.Key()});
    return FirstRv(res.GetSingleCursor());
}

std::vector<WatchEvent> Store::EventsAfter(const Resource& r, std::uint64_t rv, std::size_t limit) {
    auto res = table_->ExecuteQuery(
        kEventsAfter, "$resource", ydb::Utf8{r.Key()}, "$rv", rv, "$limit", static_cast<std::uint64_t>(limit)
    );
    std::vector<WatchEvent> out;
    for (auto row : res.GetSingleCursor()) {
        WatchEvent e;
        e.rv = row.Get<std::optional<std::uint64_t>>("rv").value_or(0);
        e.type = row.Get<std::optional<ydb::Utf8>>("type").value_or(ydb::Utf8{}).GetUnderlying();
        e.ns = row.Get<std::optional<ydb::Utf8>>("ns").value_or(ydb::Utf8{}).GetUnderlying();
        e.object = json::FromString(row.Get<std::optional<std::string>>("value").value_or("{}"));
        out.push_back(std::move(e));
    }
    return out;
}

std::uint64_t Store::Compacted(const Resource& r) {
    auto res = table_->ExecuteQuery(kCompacted, "$resource", ydb::Utf8{r.Key()});
    return FirstRv(res.GetSingleCursor());
}

std::uint64_t Store::Count(const Resource& r, const std::string& ns, const std::string& after_ns, const std::string& after_name) {
    auto res = ns.empty() ? table_->ExecuteQuery(kCountAll, "$resource", ydb::Utf8{r.Key()}, "$after_ns", ydb::Utf8{after_ns}, "$after_name", ydb::Utf8{after_name})
                          : table_->ExecuteQuery(kCountNs, "$resource", ydb::Utf8{r.Key()}, "$ns", ydb::Utf8{ns}, "$after_name", ydb::Utf8{after_name});
    for (auto row : res.GetSingleCursor()) return row.Get<std::optional<std::uint64_t>>("n").value_or(0);
    return 0;
}

std::uint64_t Store::OldestEvent(const Resource& r) {
    auto res = table_->ExecuteQuery(kOldestEvent, "$resource", ydb::Utf8{r.Key()});
    return FirstRv(res.GetSingleCursor());
}

}  // namespace yapi
