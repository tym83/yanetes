#include <selectors.hpp>

#include <algorithm>
#include <cctype>

#include <status.hpp>

namespace yapi {

namespace json = userver::formats::json;

namespace {

std::string Trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return std::string(s);
}

// Splits on commas that are not inside parentheses.
std::vector<std::string> SplitTerms(const std::string& text) {
    std::vector<std::string> out;
    int depth = 0;
    std::string cur;
    for (char c : text) {
        if (c == '(') ++depth;
        if (c == ')') --depth;
        if (c == ',' && depth == 0) {
            out.push_back(Trim(cur));
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!Trim(cur).empty()) out.push_back(Trim(cur));
    return out;
}

std::vector<std::string> ParseSet(const std::string& text) {
    auto open = text.find('('), close = text.rfind(')');
    if (open == std::string::npos || close == std::string::npos || close < open) {
        throw BadRequest("unable to parse requirement: expected a set of values in parentheses: " + text);
    }
    std::vector<std::string> values;
    std::string inner = text.substr(open + 1, close - open - 1);
    std::string cur;
    for (char c : inner + ",") {
        if (c == ',') {
            if (!Trim(cur).empty()) values.push_back(Trim(cur));
            cur.clear();
        } else {
            cur += c;
        }
    }
    return values;
}

json::Value AtPath(const json::Value& object, const std::string& path) {
    json::Value v = object;
    std::size_t start = 0;
    while (start <= path.size()) {
        auto dot = path.find('.', start);
        auto part = path.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        if (!v.IsObject() || !v.HasMember(part)) return json::Value();
        v = v[part];
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    return v;
}

std::string Scalar(const json::Value& v) {
    if (v.IsMissing() || v.IsNull()) return "";
    if (v.IsString()) return v.As<std::string>();
    if (v.IsBool()) return v.As<bool>() ? "true" : "false";
    if (v.IsInt64()) return std::to_string(v.As<std::int64_t>());
    if (v.IsDouble()) return std::to_string(v.As<double>());
    return "";
}

}  // namespace

LabelSelector LabelSelector::Parse(const std::string& text) {
    LabelSelector s;
    for (const auto& term : SplitTerms(text)) {
        if (term.empty()) continue;
        if (term[0] == '!') {
            s.terms_.push_back({Trim(term.substr(1)), Op::kNotExists, {}});
            continue;
        }
        if (auto p = term.find("!="); p != std::string::npos) {
            s.terms_.push_back({Trim(term.substr(0, p)), Op::kNotEq, {Trim(term.substr(p + 2))}});
            continue;
        }
        if (auto p = term.find("=="); p != std::string::npos) {
            s.terms_.push_back({Trim(term.substr(0, p)), Op::kEq, {Trim(term.substr(p + 2))}});
            continue;
        }
        if (auto p = term.find('='); p != std::string::npos) {
            s.terms_.push_back({Trim(term.substr(0, p)), Op::kEq, {Trim(term.substr(p + 1))}});
            continue;
        }
        if (auto p = term.find(" notin "); p != std::string::npos) {
            s.terms_.push_back({Trim(term.substr(0, p)), Op::kNotIn, ParseSet(term.substr(p))});
            continue;
        }
        if (auto p = term.find(" in "); p != std::string::npos) {
            s.terms_.push_back({Trim(term.substr(0, p)), Op::kIn, ParseSet(term.substr(p))});
            continue;
        }
        if (term.find_first_of(" ()") != std::string::npos) {
            throw BadRequest("unable to parse requirement: " + term);
        }
        s.terms_.push_back({term, Op::kExists, {}});
    }
    return s;
}

bool LabelSelector::Matches(const json::Value& labels) const {
    for (const auto& t : terms_) {
        bool has = labels.IsObject() && labels.HasMember(t.key);
        std::string value = has ? Scalar(labels[t.key]) : "";
        bool in = std::find(t.values.begin(), t.values.end(), value) != t.values.end();
        switch (t.op) {
            case Op::kExists:
                if (!has) return false;
                break;
            case Op::kNotExists:
                if (has) return false;
                break;
            case Op::kEq:
            case Op::kIn:
                if (!has || !in) return false;
                break;
            case Op::kNotEq:
            case Op::kNotIn:
                if (has && in) return false;
                break;
        }
    }
    return true;
}

FieldSelector FieldSelector::Parse(const std::string& text) {
    FieldSelector s;
    for (const auto& term : SplitTerms(text)) {
        if (term.empty()) continue;
        if (auto p = term.find("!="); p != std::string::npos) {
            s.terms_.push_back({Trim(term.substr(0, p)), false, Trim(term.substr(p + 2))});
        } else if (auto p2 = term.find("=="); p2 != std::string::npos) {
            s.terms_.push_back({Trim(term.substr(0, p2)), true, Trim(term.substr(p2 + 2))});
        } else if (auto p3 = term.find('='); p3 != std::string::npos) {
            s.terms_.push_back({Trim(term.substr(0, p3)), true, Trim(term.substr(p3 + 1))});
        } else {
            throw BadRequest("invalid selector: '" + text + "'; can't understand '" + term + "'");
        }
    }
    return s;
}

bool FieldSelector::Matches(const json::Value& object) const {
    for (const auto& t : terms_) {
        // An absent field is its type's zero, as in the Go structs upstream
        // selects on: spec.unschedulable=false matches a node without it.
        const auto v = AtPath(object, t.path);
        bool eq = (v.IsMissing() || v.IsNull()) ? (t.value.empty() || t.value == "false" || t.value == "0")
                                                : Scalar(v) == t.value;
        if (eq != t.equal) return false;
    }
    return true;
}

std::string FieldSelector::Pinned(const std::string& path) const {
    for (const auto& t : terms_) {
        if (t.equal && t.path == path) return t.value;
    }
    return "";
}

}  // namespace yapi
