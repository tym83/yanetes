#pragma once

#include <string>
#include <vector>

#include <userver/formats/json/value.hpp>

namespace yapi::bodies {

using userver::formats::json::Value;

// A request body as the server reads it, with what a strict reader would
// have refused: keys given twice.
struct Parsed {
    Value value;
    // JSON: "spec.replicas" paths; the last value is kept, as Go keeps it.
    std::vector<std::string> duplicate_fields;
    // YAML: upstream's words for the first duplicate key, empty if none.
    std::string yaml_duplicate;
};

// JSON that may repeat keys; throws std::runtime_error on bad syntax.
Parsed Json(const std::string& body);

// YAML (a single document); throws std::runtime_error on bad syntax.
Parsed Yaml(const std::string& body);

bool IsYaml(const std::string& content_type);

}  // namespace yapi::bodies
