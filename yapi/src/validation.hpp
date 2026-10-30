#pragma once

#include <userver/formats/json/value.hpp>

#include <registry.hpp>

namespace yapi::validation {

// The checks of upstream's validation packages that clients rely on,
// worded the same way. Throws ApiError (422) listing every problem.
void Validate(const Resource& r, const userver::formats::json::Value& object);

}  // namespace yapi::validation
