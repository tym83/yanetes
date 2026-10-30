#include <schema.hpp>

#include <regex>
#include <set>

#include <userver/formats/json/serialize.hpp>
#include <userver/formats/json/value_builder.hpp>

namespace yapi::schema {

namespace json = userver::formats::json;
using json::ValueBuilder;

namespace {

const std::set<std::string> kObjectMeta = {
    "name", "generateName", "namespace", "selfLink", "uid", "resourceVersion", "generation", "creationTimestamp",
    "deletionTimestamp", "deletionGracePeriodSeconds", "labels", "annotations", "ownerReferences", "finalizers",
    "managedFields",
};

std::string Show(const Value& v) {
    if (v.IsString()) return "\"" + v.As<std::string>() + "\"";
    return json::ToString(v);
}

std::string TypeOf(const Value& v) {
    if (v.IsString()) return "string";
    if (v.IsBool()) return "boolean";
    if (v.IsInt64() || v.IsUInt64() || v.IsInt()) return "integer";
    if (v.IsDouble()) return "number";
    if (v.IsArray()) return "array";
    if (v.IsObject()) return "object";
    return "null";
}

bool IsType(const Value& v, const std::string& type) {
    if (type == "object") return v.IsObject();
    if (type == "array") return v.IsArray();
    if (type == "string") return v.IsString();
    if (type == "boolean") return v.IsBool();
    if (type == "integer") return v.IsInt64() || v.IsUInt64() || v.IsInt();
    if (type == "number") return v.IsDouble() || v.IsInt64() || v.IsUInt64() || v.IsInt();
    return true;
}

void Invalid(Findings& f, const std::string& path, const Value& v, const std::string& why) {
    f.problems.push_back({path, "FieldValueInvalid", path + ": Invalid value: " + Show(v) + ": " + why});
}

}  // namespace

Value Schema::Resolve(const Value& schema) const {
    if (!schema.IsObject()) return schema;
    if (components_ != nullptr) {
        auto ref = schema["$ref"].As<std::string>("");
        if (ref.empty() && schema["allOf"].IsArray() && schema["allOf"].GetSize() == 1) {
            ref = schema["allOf"][0]["$ref"].As<std::string>("");
        }
        const std::string prefix = "#/components/schemas/";
        if (ref.starts_with(prefix)) {
            auto it = components_->find(ref.substr(prefix.size()));
            if (it != components_->end()) return it->second;
        }
    }
    return schema;
}

Value Schema::Apply(const Value& object, Findings& findings) const {
    if (Empty()) return object;
    return Walk(root_, object, "", findings, true);
}

Value Schema::Walk(const Value& raw, const Value& value, const std::string& path, Findings& f, bool root) const {
    const Value s = Resolve(raw);
    if (!s.IsObject() || s.IsEmpty()) return value;
    // Built-in types are defaulted and validated by the server's own code;
    // their schemas only say which fields exist.
    const bool typed = components_ != nullptr;
    if (value.IsNull()) {
        // Non-nullable nulls are dropped before validation, as upstream prunes them.
        return s["nullable"].As<bool>(false) || typed ? value : Value();
    }
    if (s["x-kubernetes-int-or-string"].As<bool>(false) || s["format"].As<std::string>("") == "int-or-string") {
        if (!typed && !(value.IsString() || IsType(value, "integer"))) {
            Invalid(f, path, value, path + " in body must be of type integer,string: " + Show(value));
        }
        return value;
    }
    auto type = s["type"].As<std::string>("");
    const bool looks_object = s.HasMember("properties") || s.HasMember("additionalProperties");
    if (type == "object" || (type.empty() && looks_object && value.IsObject())) {
        if (!value.IsObject()) {
            if (!typed) Invalid(f, path, value, path + " in body must be of type object: " + Show(value));
            return value;
        }
        const bool preserve = s["x-kubernetes-preserve-unknown-fields"].As<bool>(false);
        const bool embedded = s["x-kubernetes-embedded-resource"].As<bool>(false);
        const auto props = s["properties"];
        const auto additional = s["additionalProperties"];
        ValueBuilder out(json::Type::kObject);
        for (auto it = value.begin(); it != value.end(); ++it) {
            const auto key = it.GetName();
            const auto child = path.empty() ? key : path + "." + key;
            if ((root || embedded) && (key == "apiVersion" || key == "kind" || key == "metadata") &&
                !(props.IsObject() && props.HasMember(key) && key != "metadata")) {
                if (key == "metadata" && it->IsObject()) {
                    ValueBuilder meta(json::Type::kObject);
                    for (auto m = it->begin(); m != it->end(); ++m) {
                        if (kObjectMeta.count(m.GetName())) {
                            meta[m.GetName()] = *m;
                        } else {
                            f.unknown.push_back(child + "." + m.GetName());
                        }
                    }
                    out[key] = meta;
                } else {
                    out[key] = *it;
                }
                continue;
            }
            Value next;
            if (props.IsObject() && props.HasMember(key)) {
                next = Walk(props[key], *it, child, f, false);
            } else if (additional.IsObject()) {
                next = Walk(additional, *it, child, f, false);
            } else if ((additional.IsBool() && additional.As<bool>()) || preserve) {
                next = *it;
            } else {
                f.unknown.push_back(child);
                continue;
            }
            if (!next.IsMissing()) out[key] = next;
        }
        if (!typed && props.IsObject()) {
            for (auto it = props.begin(); it != props.end(); ++it) {
                const auto prop = Resolve(*it);
                if (!out.HasMember(it.GetName()) && prop.HasMember("default")) {
                    const auto child = path.empty() ? it.GetName() : path + "." + it.GetName();
                    auto defaulted = Walk(prop, prop["default"], child, f, false);
                    if (!defaulted.IsMissing()) out[it.GetName()] = defaulted;
                }
            }
        }
        auto result = out.ExtractValue();
        if (!typed) {
            if (s["required"].IsArray()) {
                for (const auto& req : s["required"]) {
                    auto name = req.As<std::string>("");
                    if (!result.HasMember(name)) {
                        auto child = path.empty() ? name : path + "." + name;
                        f.problems.push_back({child, "FieldValueRequired", child + ": Required value"});
                    }
                }
            }
            if (s.HasMember("minProperties") && result.GetSize() < s["minProperties"].As<std::size_t>()) {
                Invalid(f, path, value, path + " in body should have at least " + std::to_string(s["minProperties"].As<std::size_t>()) + " properties");
            }
            if (s.HasMember("maxProperties") && result.GetSize() > s["maxProperties"].As<std::size_t>()) {
                Invalid(f, path, value, path + " in body should have at most " + std::to_string(s["maxProperties"].As<std::size_t>()) + " properties");
            }
        }
        return result;
    }
    if (type == "array" || (type.empty() && s.HasMember("items") && value.IsArray())) {
        if (!value.IsArray()) {
            if (!typed) Invalid(f, path, value, path + " in body must be of type array: " + Show(value));
            return value;
        }
        ValueBuilder out(json::Type::kArray);
        std::size_t i = 0;
        for (const auto& item : value) {
            auto next = Walk(s["items"], item, path + "[" + std::to_string(i++) + "]", f, false);
            out.PushBack(next.IsMissing() ? item : next);
        }
        if (!typed) {
            if (s.HasMember("minItems") && value.GetSize() < s["minItems"].As<std::size_t>()) {
                Invalid(f, path, value, path + " in body should have at least " + std::to_string(s["minItems"].As<std::size_t>()) + " items");
            }
            if (s.HasMember("maxItems") && value.GetSize() > s["maxItems"].As<std::size_t>()) {
                f.problems.push_back({path, "FieldValueTooMany", path + ": Too many: " + std::to_string(value.GetSize()) + ": must have at most " + std::to_string(s["maxItems"].As<std::size_t>()) + " items"});
            }
        }
        return out.ExtractValue();
    }
    if (typed) return value;
    if (!type.empty() && !IsType(value, type)) {
        Invalid(f, path, value, path + " in body must be of type " + type + ": \"" + TypeOf(value) + "\"");
        return value;
    }
    if (s["enum"].IsArray()) {
        bool found = false;
        std::string allowed;
        for (const auto& e : s["enum"]) {
            found = found || e == value;
            allowed += (allowed.empty() ? "" : ", ") + Show(e);
        }
        if (!found) {
            f.problems.push_back({path, "FieldValueNotSupported", path + ": Unsupported value: " + Show(value) + ": supported values: " + allowed});
        }
    }
    if (value.IsString()) {
        auto str = value.As<std::string>();
        if (s.HasMember("maxLength") && str.size() > s["maxLength"].As<std::size_t>()) {
            f.problems.push_back({path, "FieldValueTooLong", path + ": Too long: may not be more than " + std::to_string(s["maxLength"].As<std::size_t>()) + " bytes"});
        }
        if (s.HasMember("minLength") && str.size() < s["minLength"].As<std::size_t>()) {
            Invalid(f, path, value, path + " in body should be at least " + std::to_string(s["minLength"].As<std::size_t>()) + " chars long");
        }
        if (s.HasMember("pattern")) {
            try {
                if (!std::regex_search(str, std::regex(s["pattern"].As<std::string>()))) {
                    Invalid(f, path, value, path + " in body should match '" + s["pattern"].As<std::string>() + "'");
                }
            } catch (const std::regex_error&) {
            }
        }
    }
    if (IsType(value, "number")) {
        auto n = value.As<double>();
        if (s.HasMember("minimum")) {
            auto m = s["minimum"].As<double>();
            if (n < m || (s["exclusiveMinimum"].As<bool>(false) && n == m)) {
                Invalid(f, path, value, path + " in body should be greater than or equal to " + json::ToString(s["minimum"]));
            }
        }
        if (s.HasMember("maximum")) {
            auto m = s["maximum"].As<double>();
            if (n > m || (s["exclusiveMaximum"].As<bool>(false) && n == m)) {
                Invalid(f, path, value, path + " in body should be less than or equal to " + json::ToString(s["maximum"]));
            }
        }
    }
    return value;
}

std::string Message(const std::vector<Problem>& problems) {
    if (problems.size() == 1) return problems[0].message;
    std::string out = "[";
    for (std::size_t i = 0; i < problems.size(); ++i) out += (i ? ", " : "") + problems[i].message;
    return out + "]";
}

namespace {

// Just enough of a JSON reader to see every key of every object, which a
// DOM parser keeps only once.
class Scanner {
public:
    explicit Scanner(const std::string& s) : s_(s) {}

    void Value(const std::string& path, std::vector<std::string>& dups) {
        Skip();
        if (pos_ >= s_.size()) return;
        char c = s_[pos_];
        if (c == '{') {
            ++pos_;
            std::set<std::string> keys;
            for (;;) {
                Skip();
                if (pos_ >= s_.size() || s_[pos_] == '}') {
                    ++pos_;
                    return;
                }
                if (s_[pos_] == ',') {
                    ++pos_;
                    continue;
                }
                auto key = String();
                Skip();
                if (pos_ < s_.size() && s_[pos_] == ':') ++pos_;
                auto child = path.empty() ? key : path + "." + key;
                if (!keys.insert(key).second) dups.push_back(child);
                Value(child, dups);
            }
        }
        if (c == '[') {
            ++pos_;
            std::size_t i = 0;
            for (;;) {
                Skip();
                if (pos_ >= s_.size() || s_[pos_] == ']') {
                    ++pos_;
                    return;
                }
                if (s_[pos_] == ',') {
                    ++pos_;
                    continue;
                }
                Value(path + "[" + std::to_string(i++) + "]", dups);
            }
        }
        if (c == '"') {
            String();
            return;
        }
        while (pos_ < s_.size() && s_[pos_] != ',' && s_[pos_] != '}' && s_[pos_] != ']') ++pos_;
    }

private:
    void Skip() {
        while (pos_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[pos_]))) ++pos_;
    }

    std::string String() {
        std::string out;
        if (pos_ >= s_.size() || s_[pos_] != '"') return out;
        ++pos_;
        while (pos_ < s_.size() && s_[pos_] != '"') {
            if (s_[pos_] == '\\' && pos_ + 1 < s_.size()) {
                out += s_[pos_ + 1];
                pos_ += 2;
                continue;
            }
            out += s_[pos_++];
        }
        ++pos_;
        return out;
    }

    const std::string& s_;
    std::size_t pos_ = 0;
};

}  // namespace

std::vector<std::string> DuplicateFields(const std::string& body) {
    std::vector<std::string> dups;
    if (body.empty() || body[0] == 'k') return dups;  // protobuf has no duplicate keys
    Scanner(body).Value("", dups);
    return dups;
}

}  // namespace yapi::schema
