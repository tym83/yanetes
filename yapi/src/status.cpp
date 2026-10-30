#include <status.hpp>

#include <userver/formats/json/value_builder.hpp>

namespace yapi {

namespace json = userver::formats::json;

namespace {

std::string Resource(const std::string& plural, const std::string& group) {
    return group.empty() ? plural : plural + "." + group;
}

}  // namespace

json::Value ApiError::ToStatus() const {
    json::ValueBuilder b(json::Type::kObject);
    b["kind"] = "Status";
    b["apiVersion"] = "v1";
    b["metadata"] = json::ValueBuilder(json::Type::kObject);
    b["status"] = "Failure";
    b["message"] = std::string(what());
    b["reason"] = reason_;
    b["code"] = code_;
    if (!name_.empty() || !kind_.empty() || !causes_.empty()) {
        json::ValueBuilder d(json::Type::kObject);
        if (!name_.empty()) d["name"] = name_;
        if (!group_.empty()) d["group"] = group_;
        if (!kind_.empty()) d["kind"] = kind_;
        if (!causes_.empty()) {
            json::ValueBuilder causes(json::Type::kArray);
            for (const auto& c : causes_) {
                json::ValueBuilder cb(json::Type::kObject);
                cb["reason"] = c.type;
                cb["message"] = c.message;
                if (!c.field.empty()) cb["field"] = c.field;
                causes.PushBack(cb.ExtractValue());
            }
            d["causes"] = causes;
        }
        b["details"] = d;
    }
    return b.ExtractValue();
}

ApiError NotFound(const std::string& plural, const std::string& group, const std::string& name) {
    return ApiError(404, "NotFound", Resource(plural, group) + " \"" + name + "\" not found").About(plural, group, name);
}

ApiError AlreadyExists(const std::string& plural, const std::string& group, const std::string& name) {
    return ApiError(409, "AlreadyExists", Resource(plural, group) + " \"" + name + "\" already exists")
        .About(plural, group, name);
}

ApiError Conflict(const std::string& plural, const std::string& group, const std::string& name, const std::string& why) {
    return ApiError(
               409,
               "Conflict",
               "Operation cannot be fulfilled on " + Resource(plural, group) + " \"" + name + "\": " + why
    )
        .About(plural, group, name);
}

ApiError BadRequest(const std::string& message) { return ApiError(400, "BadRequest", message); }

ApiError Invalid(const std::string& kind, const std::string& group, const std::string& name, const std::string& message) {
    ApiError e(422, "Invalid", kind + " \"" + name + "\" is invalid: " + message);
    e.About(kind, group, name);
    // Each "field: reason" of the message is a cause; kubectl prints them.
    std::string list = message;
    if (list.size() > 1 && list.front() == '[' && list.back() == ']') list = list.substr(1, list.size() - 2);
    std::size_t start = 0;
    while (start < list.size()) {
        auto end = list.find(", ", start);
        // Only split where the next item starts a new "field: " pair.
        while (end != std::string::npos && list.find(": ", end + 2) != std::string::npos &&
               list.substr(end + 2, list.find(": ", end + 2) - end - 2).find(' ') != std::string::npos) {
            end = list.find(", ", end + 2);
        }
        auto item = list.substr(start, end == std::string::npos ? std::string::npos : end - start);
        auto colon = item.find(": ");
        auto field = colon == std::string::npos ? std::string() : item.substr(0, colon);
        auto reason = item.find(": Required value") != std::string::npos ? "FieldValueRequired"
                      : item.find(": Unsupported value") != std::string::npos ? "FieldValueNotSupported"
                                                                              : "FieldValueInvalid";
        e.Cause(reason, colon == std::string::npos ? item : item.substr(colon + 2), field);
        if (end == std::string::npos) break;
        start = end + 2;
    }
    return e;
}

ApiError Gone(const std::string& message) { return ApiError(410, "Expired", message); }

ApiError MethodNotAllowed(const std::string& message) { return ApiError(405, "MethodNotAllowed", message); }

ApiError Unauthorized() { return ApiError(401, "Unauthorized", "Unauthorized"); }

ApiError Forbidden(const std::string& message) { return ApiError(403, "Forbidden", message); }

json::Value SuccessStatus() {
    json::ValueBuilder b(json::Type::kObject);
    b["kind"] = "Status";
    b["apiVersion"] = "v1";
    b["metadata"] = json::ValueBuilder(json::Type::kObject);
    b["status"] = "Success";
    return b.ExtractValue();
}

}  // namespace yapi
