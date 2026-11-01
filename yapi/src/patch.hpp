#pragma once

#include <string>

#include <userver/formats/json/value.hpp>

namespace yapi::patch {

using userver::formats::json::Value;

// RFC 7386.
Value Merge(const Value& target, const Value& patch);

// RFC 6902. Throws ApiError on a failed test or a bad path.
Value Json(const Value& target, const Value& ops);

// Kubernetes strategic merge: like a merge patch, but lists with a merge key
// are merged by that key, and the $patch, $setElementOrder,
// $deleteFromPrimitiveList and $retainKeys directives are understood. Merge
// keys come from a table of the fields that have them upstream, by name.
Value Strategic(const Value& target, const Value& patch);

// Applies the patch given its Content-Type. Throws ApiError(415) on others.
Value Apply(const std::string& content_type, const Value& target, const std::string& body);

}  // namespace yapi::patch
