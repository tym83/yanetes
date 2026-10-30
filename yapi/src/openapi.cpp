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

bool Preserves(const Value& schema) { return schema["x-kubernetes-preserve-unknown-fields"].As<bool>(false); }

// What OpenAPI v2 cannot say, taken out as upstream's ToStructuralOpenAPIV2
// does: kubectl would refuse fields a v2 schema does not list.
Value ToV2(const Value& schema) {
    if (!schema.IsObject()) return schema;
    ValueBuilder s(schema);
    for (const char* k : {"allOf", "oneOf", "anyOf", "not"}) s.Remove(k);
    bool drop_children = false;
    if (schema["nullable"].As<bool>(false)) {
        s.Remove("type");
        s.Remove("nullable");
        drop_children = true;
    }
    if (Preserves(schema)) drop_children = true;
    if (drop_children) {
        s.Remove("items");
        s.Remove("properties");
    }
    const auto type = drop_children && schema["nullable"].As<bool>(false) ? std::string() : schema["type"].As<std::string>("");
    if ((type == "array" && (drop_children || !schema.HasMember("items"))) || (type == "object" && Preserves(schema))) s.Remove("type");
    if (!drop_children) {
        if (schema["properties"].IsObject()) {
            std::vector<Value> required;
            for (const auto& name : objects::Items(schema["required"])) {
                if (!schema["properties"][name.As<std::string>("")]["nullable"].As<bool>(false)) required.push_back(name);
            }
            if (schema.HasMember("required")) {
                if (required.empty()) {
                    s.Remove("required");
                } else {
                    s["required"] = required;
                }
            }
            for (auto it = schema["properties"].begin(); it != schema["properties"].end(); ++it) s["properties"][it.GetName()] = ToV2(*it);
        }
        if (schema["items"].IsObject()) s["items"] = ToV2(schema["items"]);
    }
    if (schema["additionalProperties"].IsObject()) {
        if (schema["additionalProperties"]["nullable"].As<bool>(false)) s.Remove("required");
        s["additionalProperties"] = ToV2(schema["additionalProperties"]);
    }
    return s.ExtractValue();
}

void TypeMeta(ValueBuilder& d) {
    ValueBuilder api_version(json::Type::kObject);
    api_version["type"] = "string";
    api_version["description"] = "APIVersion defines the versioned schema of this representation of an object.";
    ValueBuilder kind(json::Type::kObject);
    kind["type"] = "string";
    kind["description"] = "Kind is a string value representing the REST resource this object represents.";
    d["properties"]["apiVersion"] = api_version;
    d["properties"]["kind"] = kind;
}

// Embedded resources get the fields every object has, as at the root.
Value Embedded(const Value& schema, const std::string& meta_ref, bool v2) {
    if (!schema.IsObject()) return schema;
    ValueBuilder s(schema);
    if (schema["properties"].IsObject()) {
        for (auto it = schema["properties"].begin(); it != schema["properties"].end(); ++it) {
            s["properties"][it.GetName()] = Embedded(*it, meta_ref, v2);
        }
    }
    if (schema["items"].IsObject()) s["items"] = Embedded(schema["items"], meta_ref, v2);
    if (schema["additionalProperties"].IsObject()) s["additionalProperties"] = Embedded(schema["additionalProperties"], meta_ref, v2);
    if (schema["x-kubernetes-embedded-resource"].As<bool>(false) && !(v2 && Preserves(schema))) {
        TypeMeta(s);
        ValueBuilder meta(json::Type::kObject);
        meta["$ref"] = meta_ref;
        s["properties"]["metadata"] = meta;
    }
    return s.ExtractValue();
}

// A custom resource's schema as a published definition: its own fields,
// the standard ones every object has, and the kind it belongs to.
Value Definition(const Resource& r, const std::string& meta_ref, bool v2) {
    const auto& schema = r.custom->schema;
    ValueBuilder d(json::Type::kObject);
    if (!schema.IsObject() || (v2 && Preserves(schema))) {
        // Nothing more: kubectl would refuse whatever is not listed.
        d["type"] = "object";
    } else {
        d = ValueBuilder(Embedded(v2 ? ToV2(schema) : schema, meta_ref, v2));
        TypeMeta(d);
        // A v3 reference takes no siblings: the description goes beside allOf.
        ValueBuilder ref(json::Type::kObject);
        ref["$ref"] = meta_ref;
        ValueBuilder meta(v2 ? ref.ExtractValue() : ValueBuilder(json::Type::kObject).ExtractValue());
        if (!v2) meta["allOf"] = std::vector<Value>{ref.ExtractValue()};
        meta["description"] = "Standard object's metadata. More info: https://git.k8s.io/community/contributors/devel/sig-architecture/api-conventions.md#metadata";
        d["properties"]["metadata"] = meta;
    }
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
        doc["definitions"][DefinitionName(*r)] = Definition(*r, "#/definitions/io.k8s.apimachinery.pkg.apis.meta.v1.ObjectMeta", true);
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
            Definition(*r, "#/components/schemas/io.k8s.apimachinery.pkg.apis.meta.v1.ObjectMeta", false);
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
        // kubectl checks a kind takes fieldValidation by its patch operation.
        ValueBuilder patch(op.ExtractValue());
        patch["operationId"] = "patch" + r->kind;
        patch["x-kubernetes-action"] = "patch";
        ValueBuilder param(json::Type::kObject);
        param["name"] = "fieldValidation";
        param["in"] = "query";
        param["schema"]["type"] = "string";
        patch["parameters"] = std::vector<Value>{param.ExtractValue()};
        doc["paths"][base + "/{name}"]["patch"] = patch;
    }
    return json::ToString(doc.ExtractValue());
}

}  // namespace yapi::openapi
