#include <patch.hpp>

#include <set>
#include <string_view>

#include <userver/formats/json/serialize.hpp>
#include <userver/formats/json/value_builder.hpp>
#include <userver/formats/serialize/common_containers.hpp>

#include <status.hpp>

namespace yapi::patch {

namespace json = userver::formats::json;

namespace {

// Lists merged by a key, and the key, as the API types' patchMergeKey tags
// say. Lists without a tag (containerStatuses, taints) are replaced whole.
// "ports" depends on what is in it.
std::string MergeKey(const std::string& field, const Value& list) {
    static const std::vector<std::pair<std::string_view, std::string_view>> kKeys = {
        {"containers", "name"},
        {"initContainers", "name"},
        {"ephemeralContainers", "name"},
        {"volumes", "name"},
        {"volumeMounts", "mountPath"},
        {"volumeDevices", "devicePath"},
        {"env", "name"},
        {"imagePullSecrets", "name"},
        {"conditions", "type"},
        {"addresses", "type"},
        {"podIPs", "ip"},
        {"hostIPs", "ip"},
        {"ownerReferences", "uid"},
        {"hostAliases", "ip"},
        {"resourceClaims", "name"},
        {"topologySpreadConstraints", "topologyKey"},
    };
    if (field == "ports") {
        for (const auto& item : list) {
            if (item.IsObject() && item.HasMember("containerPort")) return "containerPort";
        }
        return "port";
    }
    for (const auto& [name, key] : kKeys) {
        if (field == name) return std::string(key);
    }
    return "";
}

// Primitive lists that merge as sets.
bool SetList(const std::string& field) { return field == "finalizers"; }

bool SameKey(const Value& a, const Value& b, const std::string& key) {
    return a.IsObject() && b.IsObject() && a.HasMember(key) && b.HasMember(key) && a[key] == b[key];
}

Value StrategicValue(const Value& target, const Value& patch);

Value StrategicList(const std::string& field, const Value& target, const Value& patch) {
    if (!target.IsArray()) return patch;
    auto key = MergeKey(field, patch.IsArray() ? patch : target);
    if (SetList(field)) {
        json::ValueBuilder out(target);
        std::set<std::string> seen;
        for (const auto& v : target) seen.insert(json::ToString(v));
        for (const auto& v : patch) {
            if (seen.insert(json::ToString(v)).second) out.PushBack(v);
        }
        return out.ExtractValue();
    }
    if (key.empty()) {
        // Without a merge key a list is replaced, unless the patch asks to
        // replace explicitly in its own way.
        return patch;
    }
    // $patch: replace as an element replaces the whole list.
    for (const auto& item : patch) {
        if (item.IsObject() && item["$patch"].As<std::string>("") == "replace") {
            json::ValueBuilder out(json::Type::kArray);
            for (const auto& i : patch) {
                if (!(i.IsObject() && i.HasMember("$patch"))) out.PushBack(i);
            }
            return out.ExtractValue();
        }
    }
    // The patch's items in its order, then the server's other items in
    // theirs, interleaved where both sides know an item's place: what
    // upstream's normalizeElementOrder gives, so a new item comes first.
    std::vector<Value> server(target.begin(), target.end());
    auto server_index = [&](const Value& v) -> long {
        for (std::size_t i = 0; i < server.size(); ++i) {
            if (SameKey(server[i], v, key)) return static_cast<long>(i);
        }
        return -1;
    };
    std::vector<bool> touched(server.size(), false);
    std::vector<Value> left;
    for (const auto& item : patch) {
        if (!item.IsObject()) continue;
        auto at = server_index(item);
        if (at >= 0) touched[at] = true;
        if (item["$patch"].As<std::string>("") == "delete") continue;
        left.push_back(StrategicValue(at >= 0 ? server[at] : json::Value(), item));
    }
    std::vector<Value> right;
    for (std::size_t i = 0; i < server.size(); ++i) {
        if (!touched[i]) right.push_back(server[i]);
    }
    json::ValueBuilder out(json::Type::kArray);
    std::size_t i = 0, j = 0;
    while (i < left.size() && j < right.size()) {
        auto li = server_index(left[i]), ri = server_index(right[j]);
        if (li >= 0 && ri >= 0 && ri < li) {
            out.PushBack(right[j++]);
        } else {
            out.PushBack(left[i++]);
        }
    }
    for (; i < left.size(); ++i) out.PushBack(left[i]);
    for (; j < right.size(); ++j) out.PushBack(right[j]);
    return out.ExtractValue();
}

Value StrategicValue(const Value& target, const Value& patch) {
    if (!patch.IsObject()) return patch;
    auto directive = patch["$patch"].As<std::string>("");
    if (directive == "delete") return json::Value();
    // Fields in order, the target's first: a small ordered map.
    std::vector<std::pair<std::string, Value>> fields;
    auto find = [&](const std::string& name) {
        return std::find_if(fields.begin(), fields.end(), [&](const auto& f) { return f.first == name; });
    };
    if (target.IsObject() && directive != "replace") {
        for (auto it = target.begin(); it != target.end(); ++it) fields.emplace_back(it.GetName(), *it);
    }
    std::vector<std::pair<std::string, Value>> deletions;
    for (auto it = patch.begin(); it != patch.end(); ++it) {
        const auto name = it.GetName();
        if (name == "$patch" || name == "$retainKeys" || name.starts_with("$setElementOrder/")) continue;
        if (name.starts_with("$deleteFromPrimitiveList/")) {
            deletions.emplace_back(name.substr(std::string_view("$deleteFromPrimitiveList/").size()), *it);
            continue;
        }
        const auto& value = *it;
        auto existing = find(name);
        if (value.IsNull()) {
            if (existing != fields.end()) fields.erase(existing);
            continue;
        }
        auto current = existing != fields.end() ? existing->second : json::Value();
        Value merged = value.IsArray() ? StrategicList(name, current, value)
                       : value.IsObject() ? StrategicValue(current, value)
                                          : value;
        if (merged.IsMissing() || merged.IsNull()) {
            if (existing != fields.end()) fields.erase(existing);
        } else if (existing != fields.end()) {
            existing->second = merged;
        } else {
            fields.emplace_back(name, merged);
        }
    }
    for (const auto& [field, drop_list] : deletions) {
        auto existing = find(field);
        if (existing == fields.end() || !existing->second.IsArray()) continue;
        std::set<std::string> drop;
        for (const auto& v : drop_list) drop.insert(json::ToString(v));
        json::ValueBuilder kept(json::Type::kArray);
        for (const auto& v : existing->second) {
            if (!drop.count(json::ToString(v))) kept.PushBack(v);
        }
        existing->second = kept.ExtractValue();
    }
    std::optional<std::set<std::string>> retain;
    if (patch.HasMember("$retainKeys")) {
        retain.emplace();
        for (const auto& k : patch["$retainKeys"]) retain->insert(k.As<std::string>());
    }
    json::ValueBuilder out(json::Type::kObject);
    for (const auto& [name, value] : fields) {
        if (!retain || retain->count(name)) out[name] = value;
    }
    return out.ExtractValue();
}

// JSON pointer, RFC 6901.
std::vector<std::string> Pointer(const std::string& path) {
    if (path.empty()) return {};
    if (path[0] != '/') throw BadRequest("json patch: path must start with '/': " + path);
    std::vector<std::string> parts;
    std::string cur;
    for (std::size_t i = 1; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == '/') {
            std::string unescaped;
            for (std::size_t j = 0; j < cur.size(); ++j) {
                if (cur[j] == '~' && j + 1 < cur.size()) {
                    unescaped += cur[j + 1] == '1' ? '/' : '~';
                    ++j;
                } else {
                    unescaped += cur[j];
                }
            }
            parts.push_back(unescaped);
            cur.clear();
        } else {
            cur += path[i];
        }
    }
    return parts;
}

Value Get(const Value& root, const std::vector<std::string>& parts) {
    Value v = root;
    for (const auto& p : parts) {
        if (v.IsObject() && v.HasMember(p)) {
            v = v[p];
        } else if (v.IsArray()) {
            std::size_t idx = std::stoul(p);
            if (idx >= v.GetSize()) throw ApiError(422, "Invalid", "json patch: index out of range: " + p);
            v = v[idx];
        } else {
            throw ApiError(422, "Invalid", "json patch: path not found: " + p);
        }
    }
    return v;
}

// Sets (or, with no value, removes) the element at the path; "insert" adds
// into arrays instead of replacing.
Value Put(const Value& root, const std::vector<std::string>& parts, std::size_t depth, const std::optional<Value>& value, bool insert) {
    if (depth == parts.size()) return value ? *value : json::Value();
    const auto& p = parts[depth];
    bool last = depth + 1 == parts.size();
    if (root.IsArray()) {
        std::vector<Value> items(root.begin(), root.end());
        std::size_t idx = p == "-" ? items.size() : std::stoul(p);
        if (last) {
            if (!value) {
                if (idx >= items.size()) throw ApiError(422, "Invalid", "json patch: index out of range: " + p);
                items.erase(items.begin() + idx);
            } else if (insert) {
                if (idx > items.size()) throw ApiError(422, "Invalid", "json patch: index out of range: " + p);
                items.insert(items.begin() + idx, *value);
            } else {
                if (idx >= items.size()) throw ApiError(422, "Invalid", "json patch: index out of range: " + p);
                items[idx] = *value;
            }
        } else {
            if (idx >= items.size()) throw ApiError(422, "Invalid", "json patch: index out of range: " + p);
            items[idx] = Put(items[idx], parts, depth + 1, value, insert);
        }
        json::ValueBuilder out(json::Type::kArray);
        for (const auto& i : items) out.PushBack(i);
        return out.ExtractValue();
    }
    if (!root.IsObject()) throw ApiError(422, "Invalid", "json patch: path not found: " + p);
    json::ValueBuilder out(root);
    if (last) {
        if (value) {
            out[p] = *value;
        } else {
            if (!out.HasMember(p)) throw ApiError(422, "Invalid", "json patch: path not found: " + p);
            out.Remove(p);
        }
    } else {
        if (!root.HasMember(p)) throw ApiError(422, "Invalid", "json patch: path not found: " + p);
        out[p] = Put(root[p], parts, depth + 1, value, insert);
    }
    return out.ExtractValue();
}

}  // namespace

Value Merge(const Value& target, const Value& patch) {
    if (!patch.IsObject()) return patch;
    json::ValueBuilder out(target.IsObject() ? target : json::ValueBuilder(json::Type::kObject).ExtractValue());
    for (auto it = patch.begin(); it != patch.end(); ++it) {
        const auto name = it.GetName();
        if (it->IsNull()) {
            if (out.HasMember(name)) out.Remove(name);
        } else {
            out[name] = Merge(target.IsObject() && target.HasMember(name) ? target[name] : json::Value(), *it);
        }
    }
    return out.ExtractValue();
}

Value Strategic(const Value& target, const Value& patch) { return StrategicValue(target, patch); }

Value Json(const Value& target, const Value& ops) {
    if (!ops.IsArray()) throw BadRequest("json patch: the patch is not an array");
    Value doc = target;
    for (const auto& op : ops) {
        auto kind = op["op"].As<std::string>("");
        auto path = Pointer(op["path"].As<std::string>(""));
        if (kind == "add") {
            doc = Put(doc, path, 0, op["value"], true);
        } else if (kind == "remove") {
            doc = Put(doc, path, 0, std::nullopt, false);
        } else if (kind == "replace") {
            (void)Get(doc, path);
            doc = Put(doc, path, 0, op["value"], false);
        } else if (kind == "move" || kind == "copy") {
            auto from = Pointer(op["from"].As<std::string>(""));
            auto v = Get(doc, from);
            if (kind == "move") doc = Put(doc, from, 0, std::nullopt, false);
            doc = Put(doc, path, 0, v, true);
        } else if (kind == "test") {
            if (Get(doc, path) != op["value"]) throw ApiError(422, "Invalid", "json patch: test failed");
        } else {
            throw BadRequest("json patch: unknown operation \"" + kind + "\"");
        }
    }
    return doc;
}

Value Apply(const std::string& content_type, const Value& target, const std::string& body) {
    auto ct = content_type.substr(0, content_type.find(';'));
    Value p;
    try {
        p = json::FromString(body);
    } catch (const std::exception& e) {
        throw BadRequest(std::string("the patch is not valid JSON: ") + e.what());
    }
    if (ct == "application/merge-patch+json") return Merge(target, p);
    if (ct == "application/json-patch+json") return Json(target, p);
    if (ct == "application/strategic-merge-patch+json") return Strategic(target, p);
    throw ApiError(415, "UnsupportedMediaType", "the patch type " + ct + " is not supported");
}

}  // namespace yapi::patch
