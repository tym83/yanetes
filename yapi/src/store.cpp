#include <store.hpp>

#include <userver/components/component_config.hpp>
#include <userver/components/component_context.hpp>
#include <userver/formats/json/serialize.hpp>
#include <userver/formats/json/value_builder.hpp>
#include <userver/formats/serialize/common_containers.hpp>
#include <userver/logging/log.hpp>
#include <userver/ydb/component.hpp>
#include <userver/ydb/table.hpp>
#include <userver/ydb/transaction.hpp>
#include <userver/yaml_config/merge_schemas.hpp>

#include <watch.hpp>

namespace yapi {

namespace json = userver::formats::json;
namespace ydb = userver::ydb;
using userver::storages::Query;

namespace {

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
)"};

const Query kGet{
    R"(
DECLARE $resource AS Utf8; DECLARE $ns AS Utf8; DECLARE $name AS Utf8;
SELECT value FROM objects WHERE res = $resource AND ns = $ns AND name = $name;
)",
    Query::Name{"get"}};

const Query kReadForWrite{
    R"(
DECLARE $resource AS Utf8; DECLARE $ns AS Utf8; DECLARE $name AS Utf8;
SELECT rv FROM revisions WHERE res = $resource;
SELECT value FROM objects WHERE res = $resource AND ns = $ns AND name = $name;
)",
    Query::Name{"read-for-write"}};

const Query kPut{
    R"(
DECLARE $resource AS Utf8; DECLARE $ns AS Utf8; DECLARE $name AS Utf8;
DECLARE $rv AS Uint64; DECLARE $value AS String; DECLARE $type AS Utf8;
UPSERT INTO revisions (res, rv) VALUES ($resource, $rv);
UPSERT INTO objects (res, ns, name, rv, value) VALUES ($resource, $ns, $name, $rv, $value);
UPSERT INTO events (res, rv, type, ns, value) VALUES ($resource, $rv, $type, $ns, $value);
)",
    Query::Name{"put"}};

const Query kRemove{
    R"(
DECLARE $resource AS Utf8; DECLARE $ns AS Utf8; DECLARE $name AS Utf8;
DECLARE $rv AS Uint64; DECLARE $value AS String;
UPSERT INTO revisions (res, rv) VALUES ($resource, $rv);
DELETE FROM objects WHERE res = $resource AND ns = $ns AND name = $name;
UPSERT INTO events (res, rv, type, ns, value) VALUES ($resource, $rv, "DELETED"u, $ns, $value);
)",
    Query::Name{"remove"}};

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
}

Store::~Store() = default;

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

Decision Store::Write(const Resource& r, const std::string& ns, const std::string& name, const Decide& decide) {
    Decision result;
    std::uint64_t written = 0;
    userver::ydb::RetryTxSettings settings;
    settings.tx_mode = ydb::TransactionMode::kSerializableRW;
    table_->RetryTx("write", settings, [&](ydb::TxActor& tx) -> ydb::TxAction {
        written = 0;
        auto read = tx.Execute(
            kReadForWrite, "$resource", ydb::Utf8{r.Key()}, "$ns", ydb::Utf8{ns}, "$name", ydb::Utf8{name}
        );
        std::uint64_t rv = FirstRv(read.GetCursor(0));
        std::optional<json::Value> current;
        for (auto row : read.GetCursor(1)) {
            current = json::FromString(row.Get<std::optional<std::string>>("value").value_or("{}"));
        }
        result = decide(current);
        if (result.action == Decision::Action::kKeep) {
            if (current) result.object = *current;
            return ydb::TxAction::kCommit;
        }
        std::uint64_t next = rv + 1;
        result.object = WithRv(result.object, next);
        auto value = json::ToString(result.object);
        if (result.action == Decision::Action::kPut) {
            tx.Execute(
                kPut,
                "$resource",
                ydb::Utf8{r.Key()},
                "$ns",
                ydb::Utf8{ns},
                "$name",
                ydb::Utf8{name},
                "$rv",
                next,
                "$value",
                value,
                "$type",
                ydb::Utf8{current ? "MODIFIED" : "ADDED"}
            );
        } else {
            tx.Execute(
                kRemove,
                "$resource",
                ydb::Utf8{r.Key()},
                "$ns",
                ydb::Utf8{ns},
                "$name",
                ydb::Utf8{name},
                "$rv",
                next,
                "$value",
                value
            );
        }
        written = next;
        return ydb::TxAction::kCommit;
    });
    if (written != 0 && watch_ != nullptr) watch_->Kick(r);
    return result;
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

std::uint64_t Store::OldestEvent(const Resource& r) {
    auto res = table_->ExecuteQuery(kOldestEvent, "$resource", ydb::Utf8{r.Key()});
    return FirstRv(res.GetSingleCursor());
}

}  // namespace yapi
