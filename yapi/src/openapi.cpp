#include <openapi.hpp>

#include <fstream>
#include <functional>
#include <sstream>

#include <userver/crypto/hash.hpp>
#include <userver/formats/json/serialize.hpp>
#include <userver/formats/json/value_builder.hpp>
#include <userver/formats/serialize/common_containers.hpp>

#include <objects.hpp>

namespace yapi::openapi {

namespace json = userver::formats::json;
using json::Value;
using json::ValueBuilder;

namespace {

std::string ReadFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return "";
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

Value Parse(const std::string& text) {
    try {
        return text.empty() ? Value() : json::FromString(text);
    } catch (const std::exception&) {
        return Value();
    }
}

// A custom resource's schema as a published definition: its own fields,
// the standard ones every object has, and the kind it belongs to.
Value Definition(const Resource& r, const std::string& meta_ref) {
    ValueBuilder d(r.custom->schema.IsObject() ? r.custom->schema : ValueBuilder(json::Type::kObject).ExtractValue());
    if (!d.HasMember("type")) d["type"] = "object";
    ValueBuilder api_version(json::Type::kObject);
    api_version["type"] = "string";
    api_version["description"] = "APIVersion defines the versioned schema of this representation of an object.";
    ValueBuilder kind(json::Type::kObject);
    kind["type"] = "string";
    kind["description"] = "Kind is a string value representing the REST resource this object represents.";
    ValueBuilder meta(json::Type::kObject);
    meta["$ref"] = meta_ref;
    if (!d["properties"].HasMember("apiVersion")) d["properties"]["apiVersion"] = api_version;
    if (!d["properties"].HasMember("kind")) d["properties"]["kind"] = kind;
    d["properties"]["metadata"] = meta;
    ValueBuilder gvk(json::Type::kObject);
    gvk["group"] = r.group;
    gvk["version"] = r.version;
    gvk["kind"] = r.kind;
    d["x-kubernetes-group-version-kind"] = std::vector<Value>{gvk.ExtractValue()};
    return d.ExtractValue();
}

std::vector<const Resource*> Custom(const Registry& registry) {
    std::vector<const Resource*> out;
    for (const auto* r : registry.All()) {
        if (r->custom) out.push_back(r);
    }
    return out;
}

}  // namespace

std::string DefinitionName(const Resource& r) {
    std::vector<std::string> labels;
    std::stringstream ss(r.group);
    for (std::string l; std::getline(ss, l, '.');) labels.push_back(l);
    std::string reversed;
    for (auto it = labels.rbegin(); it != labels.rend(); ++it) reversed += (reversed.empty() ? "" : ".") + *it;
    return reversed + "." + r.version + "." + r.kind;
}

Documents::Documents(std::string dir) : dir_(std::move(dir)) {
    v2_ = Parse(ReadFile(dir_ + "/swagger.json"));
    v3_index_ = Parse(ReadFile(dir_ + "/v3/index.json"));
}

std::string Documents::V2(const Registry& registry) const {
    auto custom = Custom(registry);
    if (custom.empty()) return v2_.IsMissing() ? std::string() : json::ToString(v2_);
    ValueBuilder doc(v2_.IsObject() ? v2_ : ValueBuilder(json::Type::kObject).ExtractValue());
    for (const auto* r : custom) {
        doc["definitions"][DefinitionName(*r)] = Definition(*r, "#/definitions/io.k8s.apimachinery.pkg.apis.meta.v1.ObjectMeta");
    }
    return json::ToString(doc.ExtractValue());
}

std::string Documents::V3Index(const Registry& registry) const {
    ValueBuilder index(v3_index_.IsObject() ? v3_index_ : ValueBuilder(json::Type::kObject).ExtractValue());
    std::map<std::string, std::string> hashes;
    for (const auto* r : Custom(registry)) {
        auto key = "apis/" + r->group + "/" + r->version;
        hashes[key] += json::ToString(r->custom->schema) + r->kind;
    }
    for (const auto& [key, text] : hashes) {
        auto hash = userver::crypto::hash::Sha256(text, userver::crypto::hash::OutputEncoding::kHex).substr(0, 32);
        index["paths"][key]["serverRelativeURL"] = "/openapi/v3/" + key + "?hash=" + hash;
    }
    return json::ToString(index.ExtractValue());
}

std::optional<std::string> Documents::V3(const Registry& registry, const std::string& path) const {
    std::vector<const Resource*> mine;
    for (const auto* r : Custom(registry)) {
        if ("apis/" + r->group + "/" + r->version == path) mine.push_back(r);
    }
    if (mine.empty()) {
        std::string file = path;
        for (std::size_t p = 0; (p = file.find('/', p)) != std::string::npos;) file.replace(p, 1, "__");
        auto body = ReadFile(dir_ + "/v3/" + file + "_openapi.json");
        if (body.empty()) return std::nullopt;
        return body;
    }
    // ObjectMeta and what it refers to come along from the core document.
    const auto core = Parse(ReadFile(dir_ + "/v3/api__v1_openapi.json"));
    ValueBuilder doc(json::Type::kObject);
    doc["openapi"] = "3.0.0";
    doc["info"]["title"] = "Kubernetes CRD Swagger";
    doc["info"]["version"] = "v0.1.0";
    doc["paths"] = ValueBuilder(json::Type::kObject);
    const auto core_schemas = core["components"]["schemas"];
    if (core_schemas.IsObject()) {
        for (auto it = core_schemas.begin(); it != core_schemas.end(); ++it) {
            if (it.GetName().starts_with("io.k8s.apimachinery.pkg.apis.meta.v1.")) doc["components"]["schemas"][it.GetName()] = *it;
        }
    }
    for (const auto* r : mine) {
        doc["components"]["schemas"][DefinitionName(*r)] =
            Definition(*r, "#/components/schemas/io.k8s.apimachinery.pkg.apis.meta.v1.ObjectMeta");
        // kubectl explain finds a kind by a path that serves it.
        auto base = "/apis/" + r->group + "/" + r->version + (r->namespaced ? "/namespaces/{namespace}/" : "/") + r->plural;
        ValueBuilder ref(json::Type::kObject);
        ref["$ref"] = "#/components/schemas/" + DefinitionName(*r);
        ValueBuilder op(json::Type::kObject);
        op["operationId"] = "read" + r->kind;
        op["responses"]["200"]["description"] = "OK";
        op["responses"]["200"]["content"]["application/json"]["schema"] = ref;
        op["x-kubernetes-action"] = "get";
        ValueBuilder gvk(json::Type::kObject);
        gvk["group"] = r->group;
        gvk["version"] = r->version;
        gvk["kind"] = r->kind;
        op["x-kubernetes-group-version-kind"] = gvk;
        doc["paths"][base + "/{name}"]["get"] = op;
    }
    return json::ToString(doc.ExtractValue());
}

}  // namespace yapi::openapi
