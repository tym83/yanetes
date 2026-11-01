#pragma once

#include <userver/formats/json/value.hpp>

#include <registry.hpp>

namespace yapi {

class Store;

namespace admission {

using userver::formats::json::Value;

// What upstream's built-in admission plugins do to a new object before it
// is stored: the service account token volume, default tolerations, the
// priority of a priority class, the not-ready taint of a new node.
// Throws ApiError when the object may not be created.
Value Admit(Store& store, const Resource& r, Value object);

}  // namespace admission
}  // namespace yapi
