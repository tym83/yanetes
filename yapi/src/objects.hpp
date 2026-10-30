#pragma once

#include <string>

#include <userver/formats/json/value.hpp>

#include <registry.hpp>

namespace yapi::objects {

using userver::formats::json::Value;

std::string NewUid();
std::string Now();  // RFC 3339, seconds, UTC

// Fills what the server owns on create: name from generateName, namespace,
// uid, timestamps, generation and the kind's defaults. Throws ApiError.
Value PrepareCreate(const Resource& r, Value object, const std::string& ns);

// The object an update stores, given what was stored and what came in. A
// status write changes only status; a main write keeps the stored status.
Value PrepareUpdate(const Resource& r, const Value& stored, Value incoming, bool status_write);

// Defaults the kind needs before anything reads it: the kubelet does not
// guess a pod's restartPolicy.
Value Defaults(const Resource& r, Value object);

std::string Name(const Value& object);
std::string Namespace(const Value& object);
bool HasFinalizers(const Value& object);
bool Deleting(const Value& object);

}  // namespace yapi::objects
