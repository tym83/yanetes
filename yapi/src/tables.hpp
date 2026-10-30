#pragma once

#include <string>
#include <vector>

#include <userver/formats/json/value.hpp>

#include <registry.hpp>

namespace yapi::tables {

using userver::formats::json::Value;

// Whether the client asked for a meta.k8s.io Table, as kubectl get does.
bool Wanted(const std::string& accept);

// The objects as a Table, with the columns upstream prints for the kind.
// include_object is None, Object or Metadata, the last one by default.
Value Make(
    const Resource& r,
    const std::vector<Value>& items,
    bool with_columns,
    const std::string& include_object,
    const std::string& resource_version,
    const std::string& continue_token
);

// Go's duration.HumanDuration, as the Age column shows it.
std::string HumanDuration(std::int64_t seconds);

}  // namespace yapi::tables
