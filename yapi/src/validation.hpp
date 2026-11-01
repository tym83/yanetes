#pragma once

#include <userver/formats/json/value.hpp>

#include <registry.hpp>

namespace yapi::validation {

// The checks of upstream's validation packages that clients rely on,
// worded the same way. Throws ApiError (422) listing every problem.
void Validate(const Resource& r, const userver::formats::json::Value& object);

// What an update may not change: data of an immutable ConfigMap or Secret,
// and immutable itself once set; a PriorityClass's value and policy.
void ValidateUpdate(const Resource& r, const userver::formats::json::Value& old_object, const userver::formats::json::Value& object);

}  // namespace yapi::validation
