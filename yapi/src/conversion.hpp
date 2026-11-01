#pragma once

#include <string>

#include <userver/formats/json/value.hpp>

#include <registry.hpp>

namespace yapi::conversion {

using userver::formats::json::Value;

// Built-in resources served in two shapes from one storage: events.k8s.io
// events over core events, autoscaling/v1 autoscalers over v2. Objects are
// stored in the shape of the resource that owns the key.
bool Converted(const Resource& r);
Value ToServed(const Resource& r, const Value& stored);
Value ToStored(const Resource& r, const Value& served);

// The stored path of a field selector term: core events select on "source"
// meaning source.component or else the reporting component, events.k8s.io
// names fields its own way.
std::string StoredField(const Resource& r, const std::string& path);

}  // namespace yapi::conversion
