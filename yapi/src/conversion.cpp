#include <conversion.hpp>

#include <userver/formats/json/value_builder.hpp>
#include <userver/formats/serialize/common_containers.hpp>

#include <objects.hpp>

namespace yapi::conversion {

namespace json = userver::formats::json;
using json::ValueBuilder;

namespace {

bool EventsV1(const Resource& r) { return r.group == "events.k8s.io"; }
bool AutoscalingV1(const Resource& r) { return r.group == "autoscaling" && r.version == "v1"; }

void Copy(ValueBuilder& to, const char* to_key, const Value& from, const char* from_key) {
    if (from.HasMember(from_key) && !from[from_key].IsNull()) to[to_key] = from[from_key];
}

// core/v1 Event -> events.k8s.io/v1 Event
Value EventFromCore(const Value& e) {
    ValueBuilder b(json::Type::kObject);
    b["apiVersion"] = "events.k8s.io/v1";
    b["kind"] = "Event";
    b["metadata"] = e["metadata"];
    Copy(b, "eventTime", e, "eventTime");
    Copy(b, "series", e, "series");
    Copy(b, "reportingController", e, "reportingComponent");
    Copy(b, "reportingInstance", e, "reportingInstance");
    Copy(b, "action", e, "action");
    Copy(b, "reason", e, "reason");
    Copy(b, "regarding", e, "involvedObject");
    Copy(b, "related", e, "related");
    Copy(b, "note", e, "message");
    Copy(b, "type", e, "type");
    Copy(b, "deprecatedSource", e, "source");
    Copy(b, "deprecatedFirstTimestamp", e, "firstTimestamp");
    Copy(b, "deprecatedLastTimestamp", e, "lastTimestamp");
    Copy(b, "deprecatedCount", e, "count");
    if (!b.HasMember("eventTime")) b["eventTime"] = Value();
    return b.ExtractValue();
}

// events.k8s.io/v1 Event -> core/v1 Event
Value EventToCore(const Value& e) {
    ValueBuilder b(json::Type::kObject);
    b["apiVersion"] = "v1";
    b["kind"] = "Event";
    b["metadata"] = e["metadata"];
    Copy(b, "eventTime", e, "eventTime");
    Copy(b, "series", e, "series");
    Copy(b, "reportingComponent", e, "reportingController");
    Copy(b, "reportingInstance", e, "reportingInstance");
    Copy(b, "action", e, "action");
    Copy(b, "reason", e, "reason");
    Copy(b, "involvedObject", e, "regarding");
    Copy(b, "related", e, "related");
    Copy(b, "message", e, "note");
    Copy(b, "type", e, "type");
    Copy(b, "source", e, "deprecatedSource");
    Copy(b, "firstTimestamp", e, "deprecatedFirstTimestamp");
    Copy(b, "lastTimestamp", e, "deprecatedLastTimestamp");
    Copy(b, "count", e, "deprecatedCount");
    if (!b.HasMember("involvedObject")) b["involvedObject"] = ValueBuilder(json::Type::kObject);
    return b.ExtractValue();
}

// autoscaling/v2 -> v1: the CPU utilization target, the rest kept in the
// annotations upstream round-trips through.
Value HpaToV1(const Value& h) {
    ValueBuilder b(h);
    b["apiVersion"] = "autoscaling/v1";
    ValueBuilder spec(json::Type::kObject);
    Copy(spec, "scaleTargetRef", h["spec"], "scaleTargetRef");
    Copy(spec, "minReplicas", h["spec"], "minReplicas");
    Copy(spec, "maxReplicas", h["spec"], "maxReplicas");
    for (const auto& m : objects::Items(h["spec"]["metrics"])) {
        if (m["type"].As<std::string>("") == "Resource" && m["resource"]["name"].As<std::string>("") == "cpu" &&
            m["resource"]["target"]["type"].As<std::string>("") == "Utilization") {
            spec["targetCPUUtilizationPercentage"] = m["resource"]["target"]["averageUtilization"];
        }
    }
    b["spec"] = spec;
    ValueBuilder status(json::Type::kObject);
    Copy(status, "observedGeneration", h["status"], "observedGeneration");
    Copy(status, "lastScaleTime", h["status"], "lastScaleTime");
    status["currentReplicas"] = h["status"]["currentReplicas"].As<std::int64_t>(0);
    status["desiredReplicas"] = h["status"]["desiredReplicas"].As<std::int64_t>(0);
    for (const auto& m : objects::Items(h["status"]["currentMetrics"])) {
        if (m["type"].As<std::string>("") == "Resource" && m["resource"]["name"].As<std::string>("") == "cpu") {
            Copy(status, "currentCPUUtilizationPercentage", m["resource"]["current"], "averageUtilization");
        }
    }
    b["status"] = status;
    return b.ExtractValue();
}

Value HpaFromV1(const Value& h) {
    ValueBuilder b(h);
    b["apiVersion"] = "autoscaling/v2";
    ValueBuilder spec(json::Type::kObject);
    Copy(spec, "scaleTargetRef", h["spec"], "scaleTargetRef");
    Copy(spec, "minReplicas", h["spec"], "minReplicas");
    Copy(spec, "maxReplicas", h["spec"], "maxReplicas");
    if (h["spec"].HasMember("targetCPUUtilizationPercentage")) {
        ValueBuilder metric(json::Type::kObject);
        metric["type"] = "Resource";
        metric["resource"]["name"] = "cpu";
        metric["resource"]["target"]["type"] = "Utilization";
        metric["resource"]["target"]["averageUtilization"] = h["spec"]["targetCPUUtilizationPercentage"];
        spec["metrics"] = std::vector<Value>{metric.ExtractValue()};
    }
    b["spec"] = spec;
    ValueBuilder status(json::Type::kObject);
    Copy(status, "observedGeneration", h["status"], "observedGeneration");
    Copy(status, "lastScaleTime", h["status"], "lastScaleTime");
    Copy(status, "currentReplicas", h["status"], "currentReplicas");
    Copy(status, "desiredReplicas", h["status"], "desiredReplicas");
    b["status"] = status;
    return b.ExtractValue();
}

}  // namespace

bool Converted(const Resource& r) { return EventsV1(r) || AutoscalingV1(r); }

Value ToServed(const Resource& r, const Value& stored) {
    if (!stored.IsObject()) return stored;
    if (EventsV1(r)) return EventFromCore(stored);
    if (AutoscalingV1(r)) return HpaToV1(stored);
    return stored;
}

Value ToStored(const Resource& r, const Value& served) {
    if (!served.IsObject()) return served;
    if (EventsV1(r)) return EventToCore(served);
    if (AutoscalingV1(r)) return HpaFromV1(served);
    return served;
}

std::string StoredField(const Resource& r, const std::string& path) {
    if (r.group.empty() && r.plural == "events" && path == "source") return "source.component|reportingComponent";
    if (!EventsV1(r)) return path;
    if (path == "reportingController") return "reportingComponent";
    if (path == "note") return "message";
    if (path.starts_with("regarding.")) return "involvedObject." + path.substr(10);
    if (path.starts_with("deprecatedSource.")) return "source." + path.substr(17);
    return path;
}

}  // namespace yapi::conversion
