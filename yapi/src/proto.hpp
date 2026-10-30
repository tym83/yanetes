#pragma once

#include <memory>
#include <string>

#include <userver/formats/json/value.hpp>

namespace yapi::proto {

// Kubernetes clients send typed objects as protobuf and do not fall back to
// JSON. This turns such a body into the JSON the rest of the server works
// with, using the upstream generated.proto files loaded at startup. The
// types upstream serializes by hand (Time, Quantity, IntOrString, ...) are
// converted by hand here too.
class Decoder {
public:
    explicit Decoder(const std::string& proto_dir);
    ~Decoder();

    static bool IsProtobuf(const std::string& body) { return body.starts_with(std::string_view("k8s\0", 4)); }

    // Throws ApiError when the body is not a known object.
    userver::formats::json::Value Decode(const std::string& body) const;

    // The decoder of the server, once one is set up; nullptr before.
    static const Decoder* Instance();
    static void SetInstance(const Decoder* decoder);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Go's time.Duration.String().
std::string GoDuration(std::int64_t nanoseconds);

}  // namespace yapi::proto
