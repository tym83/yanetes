#include <proto.hpp>

#include <atomic>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <unordered_set>

#include <google/protobuf/compiler/importer.h>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/message.h>
#include <google/protobuf/reflection.h>

#include <userver/crypto/base64.hpp>
#include <userver/formats/json/serialize.hpp>
#include <userver/formats/json/value_builder.hpp>
#include <userver/logging/log.hpp>

#include <status.hpp>

namespace yapi::proto {

namespace json = userver::formats::json;
namespace pb = google::protobuf;

namespace {

std::atomic<const Decoder*> g_instance{nullptr};

// Scalar fields of the API types behind a pointer, as message.field. Go's
// protobuf writes every other scalar even when it is zero, where JSON
// leaves it out; only for these is "", 0 or false a value (storageClassName
// "" turns provisioning off, replicas 0 is not one).
std::unordered_set<std::string> g_pointer_fields;
// Embedded structs JSON inlines and protobuf keeps as fields of their own:
// a Volume's VolumeSource, a Probe's handler.
std::unordered_set<std::string> g_inline_fields;

class Errors final : public pb::compiler::MultiFileErrorCollector {
public:
    void AddError(const std::string& filename, int line, int, const std::string& message) override {
        LOG_WARNING() << "proto " << filename << ":" << line << ": " << message;
    }
};

std::string Rfc3339(std::int64_t seconds, std::int32_t nanos, bool micro) {
    std::time_t t = seconds;
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[40];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
    std::string out = buf;
    if (micro) {
        char frac[16];
        std::snprintf(frac, sizeof(frac), ".%06d", nanos / 1000);
        out += frac;
    }
    return out + "Z";
}

json::Value Convert(const pb::Message& m);

json::Value Scalar(const pb::Message& m, const pb::Reflection* r, const pb::FieldDescriptor* f, int index) {
    using T = pb::FieldDescriptor;
    bool rep = index >= 0;
    switch (f->cpp_type()) {
        case T::CPPTYPE_INT32:
            return json::ValueBuilder(rep ? r->GetRepeatedInt32(m, f, index) : r->GetInt32(m, f)).ExtractValue();
        case T::CPPTYPE_INT64:
            return json::ValueBuilder(rep ? r->GetRepeatedInt64(m, f, index) : r->GetInt64(m, f)).ExtractValue();
        case T::CPPTYPE_UINT32:
            return json::ValueBuilder(rep ? r->GetRepeatedUInt32(m, f, index) : r->GetUInt32(m, f)).ExtractValue();
        case T::CPPTYPE_UINT64:
            return json::ValueBuilder(rep ? r->GetRepeatedUInt64(m, f, index) : r->GetUInt64(m, f)).ExtractValue();
        case T::CPPTYPE_DOUBLE:
            return json::ValueBuilder(rep ? r->GetRepeatedDouble(m, f, index) : r->GetDouble(m, f)).ExtractValue();
        case T::CPPTYPE_FLOAT:
            return json::ValueBuilder(static_cast<double>(rep ? r->GetRepeatedFloat(m, f, index) : r->GetFloat(m, f)))
                .ExtractValue();
        case T::CPPTYPE_BOOL:
            return json::ValueBuilder(rep ? r->GetRepeatedBool(m, f, index) : r->GetBool(m, f)).ExtractValue();
        case T::CPPTYPE_ENUM:
            return json::ValueBuilder(rep ? r->GetRepeatedEnumValue(m, f, index) : r->GetEnumValue(m, f)).ExtractValue();
        case T::CPPTYPE_STRING: {
            auto s = rep ? r->GetRepeatedString(m, f, index) : r->GetString(m, f);
            if (f->type() == T::TYPE_BYTES) {
                return json::ValueBuilder(userver::crypto::base64::Base64Encode(s)).ExtractValue();
            }
            return json::ValueBuilder(s).ExtractValue();
        }
        case T::CPPTYPE_MESSAGE:
            return Convert(rep ? r->GetRepeatedMessage(m, f, index) : r->GetMessage(m, f));
    }
    return json::Value();
}

// Raw JSON carried in bytes, as RawExtension and FieldsV1 do.
json::Value RawJson(const std::string& raw) {
    if (raw.empty()) return json::Value();
    try {
        return json::FromString(raw);
    } catch (const std::exception&) {
        return json::Value();
    }
}

json::Value Convert(const pb::Message& m) {
    const auto* d = m.GetDescriptor();
    const auto* r = m.GetReflection();
    const auto& name = d->full_name();
    auto field = [&](const char* n) { return d->FindFieldByName(n); };

    if (name == "k8s.io.apimachinery.pkg.apis.meta.v1.Time" || name == "k8s.io.apimachinery.pkg.apis.meta.v1.MicroTime") {
        auto s = r->GetInt64(m, field("seconds"));
        auto n = r->GetInt32(m, field("nanos"));
        if (s == 0 && n == 0) return json::Value();
        return json::ValueBuilder(Rfc3339(s, n, name.ends_with("MicroTime"))).ExtractValue();
    }
    if (name == "k8s.io.apimachinery.pkg.apis.meta.v1.Duration") {
        return json::ValueBuilder(GoDuration(r->GetInt64(m, field("duration")))).ExtractValue();
    }
    if (name == "k8s.io.apimachinery.pkg.util.intstr.IntOrString") {
        if (r->GetInt64(m, field("type")) == 1) return json::ValueBuilder(r->GetString(m, field("strVal"))).ExtractValue();
        return json::ValueBuilder(r->GetInt32(m, field("intVal"))).ExtractValue();
    }
    if (name == "k8s.io.apimachinery.pkg.api.resource.Quantity") {
        return json::ValueBuilder(r->GetString(m, field("string"))).ExtractValue();
    }
    if (name == "k8s.io.apimachinery.pkg.runtime.RawExtension" || name == "k8s.io.apimachinery.pkg.apis.meta.v1.FieldsV1") {
        const auto* raw = field("raw") ? field("raw") : field("Raw");
        return raw ? RawJson(r->GetString(m, raw)) : json::Value();
    }

    json::ValueBuilder out(json::Type::kObject);
    std::vector<const pb::FieldDescriptor*> fields;
    r->ListFields(m, &fields);
    for (const auto* f : fields) {
        if (f->is_map()) {
            json::ValueBuilder map(json::Type::kObject);
            const auto* key = f->message_type()->FindFieldByName("key");
            const auto* value = f->message_type()->FindFieldByName("value");
            for (int i = 0; i < r->FieldSize(m, f); ++i) {
                const auto& entry = r->GetRepeatedMessage(m, f, i);
                auto k = Scalar(entry, entry.GetReflection(), key, -1);
                map[k.IsString() ? k.As<std::string>() : json::ToString(k)] =
                    Scalar(entry, entry.GetReflection(), value, -1);
            }
            out[std::string(f->name())] = map;
        } else if (f->is_repeated()) {
            json::ValueBuilder list(json::Type::kArray);
            for (int i = 0; i < r->FieldSize(m, f); ++i) list.PushBack(Scalar(m, r, f, i));
            out[std::string(f->name())] = list;
        } else if (f->cpp_type() == pb::FieldDescriptor::CPPTYPE_MESSAGE &&
                   g_inline_fields.count(std::string(d->full_name()) + "." + std::string(f->name()))) {
            auto v = Scalar(m, r, f, -1);
            if (v.IsObject()) {
                for (auto it = v.begin(); it != v.end(); ++it) out[it.GetName()] = *it;
            }
        } else {
            auto v = Scalar(m, r, f, -1);
            // Go writes every plain string field, JSON omits the empty ones:
            // an empty imagePullPolicy is not a policy.
            const bool zero = (v.IsString() && f->type() == pb::FieldDescriptor::TYPE_STRING && v.As<std::string>().empty()) ||
                              (v.IsBool() && !v.As<bool>()) || ((v.IsInt64() || v.IsUInt64() || v.IsInt()) && v.As<std::int64_t>() == 0) ||
                              (v.IsDouble() && v.As<double>() == 0);
            if (zero && f->cpp_type() != pb::FieldDescriptor::CPPTYPE_MESSAGE &&
                !g_pointer_fields.count(std::string(d->full_name()) + "." + std::string(f->name()))) {
                continue;
            }
            if (!v.IsNull() && !v.IsMissing()) out[std::string(f->name())] = v;
        }
    }
    return out.ExtractValue();
}

// The message of a kind: the API group's package, or meta, or the
// apiextensions types that live in their own repository.
std::vector<std::string> Candidates(const std::string& api_version, const std::string& kind) {
    auto slash = api_version.find('/');
    std::string group = slash == std::string::npos ? "" : api_version.substr(0, slash);
    std::string version = slash == std::string::npos ? api_version : api_version.substr(slash + 1);
    std::string pkg = group.empty() ? "core" : group.substr(0, group.find('.'));
    return {
        "k8s.io.api." + pkg + "." + version + "." + kind,
        "k8s.io.apimachinery.pkg.apis.meta." + version + "." + kind,
        "k8s.io.apimachinery.pkg.apis.meta.v1." + kind,
        "k8s.io.apiextensions_apiserver.pkg.apis.apiextensions." + version + "." + kind,
    };
}

}  // namespace

struct Decoder::Impl {
    pb::compiler::DiskSourceTree tree;
    Errors errors;
    pb::compiler::Importer importer{&tree, &errors};
    pb::DynamicMessageFactory factory{importer.pool()};
};

Decoder::Decoder(const std::string& proto_dir) : impl_(std::make_unique<Impl>()) {
    impl_->tree.MapPath("", proto_dir);
    std::size_t loaded = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(proto_dir)) {
        if (entry.path().extension() != ".proto") continue;
        auto rel = std::filesystem::relative(entry.path(), proto_dir).string();
        if (impl_->importer.Import(rel)) ++loaded;
    }
    for (auto [file, set] : {std::pair{"/pointer-fields.txt", &g_pointer_fields}, std::pair{"/inline-fields.txt", &g_inline_fields}}) {
        std::ifstream in(proto_dir + file);
        for (std::string line; std::getline(in, line);) {
            if (!line.empty()) set->insert(line);
        }
    }
    LOG_INFO() << "protobuf: loaded " << loaded << " files from " << proto_dir << ", " << g_pointer_fields.size()
               << " pointer fields, " << g_inline_fields.size() << " inline fields";
}

Decoder::~Decoder() = default;

json::Value Decoder::Decode(const std::string& body) const {
    const auto* unknown_desc = impl_->importer.pool()->FindMessageTypeByName("k8s.io.apimachinery.pkg.runtime.Unknown");
    if (!unknown_desc) throw ApiError(415, "UnsupportedMediaType", "protobuf is not set up on this server");
    std::unique_ptr<pb::Message> unknown(impl_->factory.GetPrototype(unknown_desc)->New());
    if (!unknown->ParseFromArray(body.data() + 4, static_cast<int>(body.size() - 4))) {
        throw BadRequest("the protobuf body does not parse");
    }
    const auto* ur = unknown->GetReflection();
    const auto& type_meta = ur->GetMessage(*unknown, unknown_desc->FindFieldByName("typeMeta"));
    const auto* tr = type_meta.GetReflection();
    auto api_version = tr->GetString(type_meta, type_meta.GetDescriptor()->FindFieldByName("apiVersion"));
    auto kind = tr->GetString(type_meta, type_meta.GetDescriptor()->FindFieldByName("kind"));
    auto raw = ur->GetString(*unknown, unknown_desc->FindFieldByName("raw"));
    for (const auto& candidate : Candidates(api_version, kind)) {
        const auto* desc = impl_->importer.pool()->FindMessageTypeByName(candidate);
        if (!desc) continue;
        std::unique_ptr<pb::Message> msg(impl_->factory.GetPrototype(desc)->New());
        if (!msg->ParseFromString(raw)) throw BadRequest("the protobuf body of " + kind + " does not parse");
        json::ValueBuilder b(Convert(*msg));
        if (!api_version.empty()) b["apiVersion"] = api_version;
        if (!kind.empty()) b["kind"] = kind;
        return b.ExtractValue();
    }
    throw ApiError(415, "UnsupportedMediaType", "no protobuf schema for " + api_version + " " + kind);
}

const Decoder* Decoder::Instance() { return g_instance.load(); }

void Decoder::SetInstance(const Decoder* decoder) { g_instance.store(decoder); }

std::string GoDuration(std::int64_t ns) {
    if (ns == 0) return "0s";
    std::string sign = ns < 0 ? "-" : "";
    std::uint64_t u = ns < 0 ? static_cast<std::uint64_t>(-ns) : static_cast<std::uint64_t>(ns);
    auto frac = [](std::uint64_t v, int digits) {
        std::string s = std::to_string(v);
        s = std::string(digits - s.size(), '0') + s;
        while (!s.empty() && s.back() == '0') s.pop_back();
        return s.empty() ? std::string() : "." + s;
    };
    if (u < 1000) return sign + std::to_string(u) + "ns";
    if (u < 1000'000) return sign + std::to_string(u / 1000) + frac(u % 1000, 3) + "µs";
    if (u < 1000'000'000) return sign + std::to_string(u / 1000'000) + frac(u % 1000'000, 6) + "ms";
    std::uint64_t secs = u / 1000'000'000, rem = u % 1000'000'000;
    std::string out;
    if (secs >= 3600) {
        out += std::to_string(secs / 3600) + "h";
        secs %= 3600;
        out += std::to_string(secs / 60) + "m";
        secs %= 60;
    } else if (secs >= 60) {
        out += std::to_string(secs / 60) + "m";
        secs %= 60;
    }
    return sign + out + std::to_string(secs) + frac(rem, 9) + "s";
}

}  // namespace yapi::proto
