#pragma once

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

}  // namespace yapi::conversion
