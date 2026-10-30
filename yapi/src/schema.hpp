#pragma once

#include <map>
#include <string>
#include <vector>

#include <userver/formats/json/value.hpp>

namespace yapi::schema {

using userver::formats::json::Value;

struct Problem {
    std::string field;   // spec.replicas
    std::string reason;  // FieldValueInvalid, FieldValueRequired, ...
    std::string message; // the whole line, as upstream words it
};

// What applying a schema found: fields it dropped and values it refused.
struct Findings {
    std::vector<std::string> unknown;  // paths of pruned fields, as "spec.foo"
    std::vector<Problem> problems;
};

// OpenAPI v3 schemas: a structural schema of a custom resource, or the
// built-in types with their $refs, resolved against `components`.
class Schema {
public:
    Schema(Value root, const std::map<std::string, Value>* components = nullptr)
        : root_(std::move(root)), components_(components) {}

    // Defaults, prunes and validates an object against the schema. The
    // root's apiVersion, kind and metadata are left alone; metadata is
    // checked against ObjectMeta's fields when known.
    Value Apply(const Value& object, Findings& findings) const;

    bool Empty() const { return !root_.IsObject() || root_.IsEmpty(); }

private:
    Value Walk(const Value& schema, const Value& value, const std::string& path, Findings& f, bool root) const;
    Value Resolve(const Value& schema) const;

    Value root_;
    const std::map<std::string, Value>* components_;
};

// Object keys that appear twice in a JSON body, as "spec.foo" paths: a
// parser keeps one of them, strict field validation rejects both.
std::vector<std::string> DuplicateFields(const std::string& body);

// Upstream's "<path>: <reason>" line for a 422 and the joined message.
std::string Message(const std::vector<Problem>& problems);

}  // namespace yapi::schema
