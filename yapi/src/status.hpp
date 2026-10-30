#pragma once

#include <stdexcept>
#include <string>
#include <vector>

#include <userver/formats/json/value.hpp>

namespace yapi {

// An error the API returns as a metav1.Status object.
class ApiError : public std::runtime_error {
public:
    ApiError(int code, std::string reason, const std::string& message)
        : std::runtime_error(message), code_(code), reason_(std::move(reason)) {}

    int Code() const { return code_; }
    const std::string& Reason() const { return reason_; }

    // Details name the object the error is about, as clients expect them.
    ApiError& About(std::string kind, std::string group, std::string name) {
        kind_ = std::move(kind);
        group_ = std::move(group);
        name_ = std::move(name);
        return *this;
    }

    // A machine-readable cause, as details.causes carries it.
    ApiError& Cause(std::string type, std::string message, std::string field = "") {
        causes_.push_back({std::move(type), std::move(message), std::move(field)});
        return *this;
    }

    userver::formats::json::Value ToStatus() const;

private:
    struct CauseEntry {
        std::string type, message, field;
    };
    std::vector<CauseEntry> causes_;
    int code_;
    std::string reason_;
    std::string kind_, group_, name_;
};

ApiError NotFound(const std::string& plural, const std::string& group, const std::string& name);
ApiError AlreadyExists(const std::string& plural, const std::string& group, const std::string& name);
ApiError Conflict(const std::string& plural, const std::string& group, const std::string& name, const std::string& why);
ApiError BadRequest(const std::string& message);
ApiError Invalid(const std::string& kind, const std::string& group, const std::string& name, const std::string& message);
ApiError Gone(const std::string& message);
ApiError MethodNotAllowed(const std::string& message);
ApiError Unauthorized();
ApiError Forbidden(const std::string& message);

// A successful delete of an object that is gone right away.
userver::formats::json::Value SuccessStatus();

}  // namespace yapi
