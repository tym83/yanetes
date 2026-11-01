#include <proto.hpp>

#include <atomic>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <unordered_map>
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
// Protobuf field names that are not the JSON ones, message.field -> name.
std::unordered_map<std::string, std::string> g_renamed_fields;

std::string JsonName(const pb::Descriptor* d, const pb::FieldDescriptor* f) {
    auto it = g_renamed_fields.find(std::string(d->full_name()) + "." + std::string(f->name()));
    return it == g_renamed_fields.end() ? std::string(f->name()) : it->second;
}

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
    // Years are four digits even before 1000, as Go writes them.
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
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
    // The schema unions of apiextensions are one JSON value each: a schema
    // or a list of them, a schema or a bool, a schema or a list of names.
    static const std::string kExt = "k8s.io.apiextensions_apiserver.pkg.apis.apiextensions.";
    if (name.starts_with(kExt)) {
        const auto type = name.substr(name.rfind('.') + 1);
        if (type == "JSON") return RawJson(r->GetString(m, field("raw")));
        if (type == "JSONSchemaPropsOrArray" || type == "JSONSchemaPropsOrBool" || type == "JSONSchemaPropsOrStringArray") {
            if (r->HasField(m, field("schema"))) return Convert(r->GetMessage(m, field("schema")));
            if (type == "JSONSchemaPropsOrBool") return json::ValueBuilder(r->GetBool(m, field("allows"))).ExtractValue();
            const auto* list = field(type == "JSONSchemaPropsOrArray" ? "jSONSchemas" : "property");
            json::ValueBuilder out(json::Type::kArray);
            for (int i = 0; i < r->FieldSize(m, list); ++i) out.PushBack(Scalar(m, r, list, i));
            return out.ExtractValue();
        }
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
            out[JsonName(d, f)] = map;
        } else if (f->is_repeated()) {
            json::ValueBuilder list(json::Type::kArray);
            for (int i = 0; i < r->FieldSize(m, f); ++i) list.PushBack(Scalar(m, r, f, i));
            out[JsonName(d, f)] = list;
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
            if (!v.IsNull() && !v.IsMissing()) out[JsonName(d, f)] = v;
        }
    }
    return out.ExtractValue();
}


// JSON back into a message, the reverse of Convert: the same hints say
// which fields are inlined and which are named differently.
bool ParseRfc3339(const std::string& text, std::int64_t& seconds, std::int32_t& nanos) {
    std::tm tm{};
    int year, mon, day, hour, min, sec;
    if (std::sscanf(text.c_str(), "%d-%d-%dT%d:%d:%d", &year, &mon, &day, &hour, &min, &sec) != 6) return false;
    tm.tm_year = year - 1900;
    tm.tm_mon = mon - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = min;
    tm.tm_sec = sec;
    seconds = timegm(&tm);
    nanos = 0;
    if (auto dot = text.find('.'); dot != std::string::npos) {
        std::string digits;
        for (std::size_t i = dot + 1; i < text.size() && std::isdigit(static_cast<unsigned char>(text[i])); ++i) digits += text[i];
        digits = (digits + "000000000").substr(0, 9);
        nanos = std::stoi(digits);
    }
    return true;
}

// Go's time.ParseDuration for what GoDuration writes: "1h2m3.5s", "150ms".
std::int64_t ParseGoDuration(const std::string& text) {
    static const std::pair<const char*, double> kUnits[] = {
        {"ns", 1}, {"us", 1e3}, {"µs", 1e3}, {"ms", 1e6}, {"s", 1e9}, {"m", 60e9}, {"h", 3600e9}};
    double total = 0;
    std::size_t i = 0;
    const bool negative = !text.empty() && text[0] == '-';
    if (negative) ++i;
    while (i < text.size()) {
        std::size_t used = 0;
        double number = std::stod(text.substr(i), &used);
        i += used;
        double scale = 0;
        std::size_t len = 0;
        for (const auto& [unit, factor] : kUnits) {
            const std::string u(unit);
            if (text.compare(i, u.size(), u) == 0 && u.size() > len) {
                scale = factor;
                len = u.size();
            }
        }
        if (len == 0) break;
        total += number * scale;
        i += len;
    }
    return static_cast<std::int64_t>(negative ? -total : total);
}

void Fill(pb::Message& m, const json::Value& v);

// The types upstream marshals by hand; false for every other message.
bool FillSpecial(pb::Message& m, const json::Value& v) {
    const auto* d = m.GetDescriptor();
    const auto* r = m.GetReflection();
    const auto& name = d->full_name();
    auto field = [&](const char* n) { return d->FindFieldByName(n); };
    if (name == "k8s.io.apimachinery.pkg.apis.meta.v1.Time" || name == "k8s.io.apimachinery.pkg.apis.meta.v1.MicroTime") {
        std::int64_t seconds = 0;
        std::int32_t nanos = 0;
        if (v.IsString() && ParseRfc3339(v.As<std::string>(), seconds, nanos)) {
            r->SetInt64(&m, field("seconds"), seconds);
            r->SetInt32(&m, field("nanos"), nanos);
        }
        return true;
    }
    if (name == "k8s.io.apimachinery.pkg.apis.meta.v1.Duration") {
        if (v.IsString()) r->SetInt64(&m, field("duration"), ParseGoDuration(v.As<std::string>()));
        return true;
    }
    if (name == "k8s.io.apimachinery.pkg.util.intstr.IntOrString") {
        if (v.IsString()) {
            r->SetInt64(&m, field("type"), 1);
            r->SetString(&m, field("strVal"), v.As<std::string>());
        } else {
            r->SetInt64(&m, field("type"), 0);
            r->SetInt32(&m, field("intVal"), v.As<std::int32_t>(0));
        }
        return true;
    }
    if (name == "k8s.io.apimachinery.pkg.api.resource.Quantity") {
        r->SetString(&m, field("string"), v.IsString() ? v.As<std::string>() : json::ToString(v));
        return true;
    }
    if (name == "k8s.io.apimachinery.pkg.runtime.RawExtension" || name == "k8s.io.apimachinery.pkg.apis.meta.v1.FieldsV1") {
        const auto* raw = field("raw") ? field("raw") : field("Raw");
        if (raw && !v.IsNull()) r->SetString(&m, raw, json::ToString(v));
        return true;
    }
    static const std::string kExt = "k8s.io.apiextensions_apiserver.pkg.apis.apiextensions.";
    if (name.starts_with(kExt)) {
        const auto type = name.substr(name.rfind('.') + 1);
        if (type == "JSON") {
            r->SetString(&m, field("raw"), json::ToString(v));
            return true;
        }
        if (type == "JSONSchemaPropsOrArray" || type == "JSONSchemaPropsOrBool" || type == "JSONSchemaPropsOrStringArray") {
            if (v.IsObject()) {
                Fill(*r->MutableMessage(&m, field("schema")), v);
                if (type == "JSONSchemaPropsOrBool") r->SetBool(&m, field("allows"), true);
            } else if (v.IsBool() && type == "JSONSchemaPropsOrBool") {
                r->SetBool(&m, field("allows"), v.As<bool>());
            } else if (v.IsArray()) {
                const auto* list = field(type == "JSONSchemaPropsOrArray" ? "jSONSchemas" : "property");
                for (const auto& item : v) {
                    if (list->cpp_type() == pb::FieldDescriptor::CPPTYPE_MESSAGE) {
                        Fill(*r->AddMessage(&m, list), item);
                    } else {
                        r->AddString(&m, list, item.As<std::string>(""));
                    }
                }
            }
            return true;
        }
    }
    return false;
}

void SetScalar(pb::Message& m, const pb::FieldDescriptor* f, const json::Value& v, bool add) {
    const auto* r = m.GetReflection();
    using T = pb::FieldDescriptor;
    switch (f->cpp_type()) {
        case T::CPPTYPE_INT32: {
            auto x = v.IsString() ? std::stoi(v.As<std::string>()) : v.As<std::int32_t>(0);
            add ? r->AddInt32(&m, f, x) : r->SetInt32(&m, f, x);
            break;
        }
        case T::CPPTYPE_INT64: {
            auto x = v.IsString() ? std::stoll(v.As<std::string>()) : v.As<std::int64_t>(0);
            add ? r->AddInt64(&m, f, x) : r->SetInt64(&m, f, x);
            break;
        }
        case T::CPPTYPE_UINT32: {
            auto x = v.As<std::uint32_t>(0);
            add ? r->AddUInt32(&m, f, x) : r->SetUInt32(&m, f, x);
            break;
        }
        case T::CPPTYPE_UINT64: {
            auto x = v.As<std::uint64_t>(0);
            add ? r->AddUInt64(&m, f, x) : r->SetUInt64(&m, f, x);
            break;
        }
        case T::CPPTYPE_DOUBLE: {
            auto x = v.As<double>(0);
            add ? r->AddDouble(&m, f, x) : r->SetDouble(&m, f, x);
            break;
        }
        case T::CPPTYPE_FLOAT: {
            auto x = static_cast<float>(v.As<double>(0));
            add ? r->AddFloat(&m, f, x) : r->SetFloat(&m, f, x);
            break;
        }
        case T::CPPTYPE_BOOL: {
            auto x = v.As<bool>(false);
            add ? r->AddBool(&m, f, x) : r->SetBool(&m, f, x);
            break;
        }
        case T::CPPTYPE_ENUM: {
            auto x = v.As<int>(0);
            add ? r->AddEnumValue(&m, f, x) : r->SetEnumValue(&m, f, x);
            break;
        }
        case T::CPPTYPE_STRING: {
            std::string x = v.IsString() ? v.As<std::string>() : json::ToString(v);
            if (f->type() == T::TYPE_BYTES) x = userver::crypto::base64::Base64Decode(x);
            add ? r->AddString(&m, f, std::move(x)) : r->SetString(&m, f, std::move(x));
            break;
        }
        case T::CPPTYPE_MESSAGE:
            break;
    }
}

void SetValue(pb::Message& m, const pb::FieldDescriptor* f, const json::Value& v, bool add) {
    const auto* r = m.GetReflection();
    if (f->cpp_type() != pb::FieldDescriptor::CPPTYPE_MESSAGE) {
        SetScalar(m, f, v, add);
        return;
    }
    auto* sub = add ? r->AddMessage(&m, f) : r->MutableMessage(&m, f);
    if (!FillSpecial(*sub, v)) Fill(*sub, v);
}

// The fields of a message by their JSON names, worked out once per type
// (per thread, so encoding never waits on a lock).
struct FieldIndex {
    std::unordered_map<std::string, const pb::FieldDescriptor*> by_name;
    std::vector<const pb::FieldDescriptor*> inlined;
};

const FieldIndex& IndexOf(const pb::Descriptor* d) {
    thread_local std::unordered_map<const pb::Descriptor*, FieldIndex> cache;
    auto [it, fresh] = cache.try_emplace(d);
    if (fresh) {
        for (int i = 0; i < d->field_count(); ++i) {
            const auto* f = d->field(i);
            if (g_inline_fields.count(std::string(d->full_name()) + "." + std::string(f->name()))) {
                it->second.inlined.push_back(f);
            } else {
                it->second.by_name[JsonName(d, f)] = f;
            }
        }
    }
    return it->second;
}

void Fill(pb::Message& m, const json::Value& v) {
    if (!v.IsObject()) return;
    const auto* d = m.GetDescriptor();
    const auto* r = m.GetReflection();
    const auto& index = IndexOf(d);
    const auto& by_name = index.by_name;
    const auto& inlined = index.inlined;
    for (auto it = v.begin(); it != v.end(); ++it) {
        if (it->IsNull()) continue;
        auto found = by_name.find(it.GetName());
        if (found == by_name.end()) continue;
        const auto* f = found->second;
        if (f->is_map()) {
            if (!it->IsObject()) continue;
            const auto* key = f->message_type()->FindFieldByName("key");
            const auto* value = f->message_type()->FindFieldByName("value");
            for (auto e = it->begin(); e != it->end(); ++e) {
                auto* entry = r->AddMessage(&m, f);
                entry->GetReflection()->SetString(entry, key, e.GetName());
                SetValue(*entry, value, *e, false);
            }
        } else if (f->is_repeated()) {
            if (!it->IsArray()) continue;
            for (const auto& item : *it) SetValue(m, f, item, true);
        } else {
            SetValue(m, f, *it, false);
        }
    }
    // An inlined struct takes the keys of its own fields from this level.
    for (const auto* f : inlined) {
        const auto* sub_d = f->message_type();
        bool any = false;
        for (int i = 0; i < sub_d->field_count() && !any; ++i) any = v.HasMember(JsonName(sub_d, sub_d->field(i)));
        if (any) Fill(*r->MutableMessage(&m, f), v);
    }
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
        "k8s.io.kube_aggregator.pkg.apis." + pkg + "." + version + "." + kind,
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
    std::ifstream renames(proto_dir + "/renamed-fields.txt");
    for (std::string line; std::getline(renames, line);) {
        auto sp = line.find(' ');
        if (sp != std::string::npos) g_renamed_fields[line.substr(0, sp)] = line.substr(sp + 1);
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

namespace {

// Encoded objects by uid, version and apiVersion: an object changes rarely
// and is listed often, so each version is encoded once, as upstream's
// cacher keeps serialized objects. Dropped wholesale when it grows big.
struct EncodedCache {
    std::mutex mutex;
    std::unordered_map<std::string, std::shared_ptr<const std::string>> items;
};
EncodedCache& Encoded() {
    static EncodedCache cache;
    return cache;
}
constexpr std::size_t kMaxEncoded = 400'000;

void AppendVarint(std::string& out, std::uint64_t v) {
    while (v >= 0x80) {
        out.push_back(static_cast<char>((v & 0x7f) | 0x80));
        v >>= 7;
    }
    out.push_back(static_cast<char>(v));
}

// A length-delimited field: tag, length, bytes.
void AppendBytes(std::string& out, int field, std::string_view bytes) {
    AppendVarint(out, (static_cast<std::uint64_t>(field) << 3) | 2);
    AppendVarint(out, bytes.size());
    out.append(bytes);
}

}  // namespace

std::shared_ptr<const std::string> Decoder::EncodeMessage(const pb::Descriptor* desc, const json::Value& object) const {
    const auto& meta = object["metadata"];
    std::string key;
    if (meta.IsObject() && meta.HasMember("uid") && meta.HasMember("resourceVersion")) {
        key = meta["uid"].As<std::string>("") + "/" + meta["resourceVersion"].As<std::string>("") + "/" + std::string(desc->full_name());
        std::lock_guard lock(Encoded().mutex);
        auto it = Encoded().items.find(key);
        if (it != Encoded().items.end()) return it->second;
    }
    std::unique_ptr<pb::Message> msg(impl_->factory.GetPrototype(desc)->New());
    Fill(*msg, object);
    auto bytes = std::make_shared<const std::string>(msg->SerializeAsString());
    if (!key.empty()) {
        std::lock_guard lock(Encoded().mutex);
        if (Encoded().items.size() >= kMaxEncoded) Encoded().items.clear();
        Encoded().items.emplace(std::move(key), bytes);
    }
    return bytes;
}

std::optional<std::string> Decoder::Encode(const json::Value& object) const {
    const auto api_version = object["apiVersion"].As<std::string>("");
    const auto kind = object["kind"].As<std::string>("");
    if (kind.empty()) return std::nullopt;
    const pb::Descriptor* desc = nullptr;
    for (const auto& candidate : Candidates(api_version, kind)) {
        if ((desc = impl_->importer.pool()->FindMessageTypeByName(candidate))) break;
    }
    const auto* unknown_desc = impl_->importer.pool()->FindMessageTypeByName("k8s.io.apimachinery.pkg.runtime.Unknown");
    if (!desc || !unknown_desc) return std::nullopt;
    // A list is its metadata and its items, fields 1 and 2 in every list
    // type: the items come from the cache of encoded objects.
    std::string raw;
    const auto* items_field = desc->FindFieldByName("items");
    if (kind.ends_with("List") && items_field && items_field->number() == 2 && items_field->message_type() &&
        desc->FindFieldByName("metadata") && desc->FindFieldByName("metadata")->number() == 1) {
        std::unique_ptr<pb::Message> list_meta(impl_->factory.GetPrototype(desc->FindFieldByName("metadata")->message_type())->New());
        Fill(*list_meta, object["metadata"]);
        AppendBytes(raw, 1, list_meta->SerializeAsString());
        for (const auto& item : object["items"]) AppendBytes(raw, 2, *EncodeMessage(items_field->message_type(), item));
    } else {
        raw = *EncodeMessage(desc, object);
    }
    std::unique_ptr<pb::Message> unknown(impl_->factory.GetPrototype(unknown_desc)->New());
    const auto* ur = unknown->GetReflection();
    auto* type_meta = ur->MutableMessage(unknown.get(), unknown_desc->FindFieldByName("typeMeta"));
    const auto* td = type_meta->GetDescriptor();
    type_meta->GetReflection()->SetString(type_meta, td->FindFieldByName("apiVersion"), api_version);
    type_meta->GetReflection()->SetString(type_meta, td->FindFieldByName("kind"), kind);
    ur->SetString(unknown.get(), unknown_desc->FindFieldByName("raw"), std::move(raw));
    ur->SetString(unknown.get(), unknown_desc->FindFieldByName("contentEncoding"), "");
    ur->SetString(unknown.get(), unknown_desc->FindFieldByName("contentType"), "");
    return std::string("k8s\0", 4) + unknown->SerializeAsString();
}

std::optional<std::string> Decoder::EncodeWatchEvent(const std::string& type, const json::Value& object) const {
    auto inner = Encode(object);
    const auto* desc = impl_->importer.pool()->FindMessageTypeByName("k8s.io.apimachinery.pkg.apis.meta.v1.WatchEvent");
    if (!inner || !desc) return std::nullopt;
    std::unique_ptr<pb::Message> event(impl_->factory.GetPrototype(desc)->New());
    const auto* r = event->GetReflection();
    r->SetString(event.get(), desc->FindFieldByName("type"), type);
    auto* raw = r->MutableMessage(event.get(), desc->FindFieldByName("object"));
    raw->GetReflection()->SetString(raw, raw->GetDescriptor()->FindFieldByName("raw"), *inner);
    auto body = event->SerializeAsString();
    const auto n = static_cast<std::uint32_t>(body.size());
    std::string frame{static_cast<char>(n >> 24), static_cast<char>(n >> 16), static_cast<char>(n >> 8), static_cast<char>(n)};
    return frame + body;
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
