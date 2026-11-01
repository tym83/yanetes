#include <bodies.hpp>

#include <cmath>
#include <regex>
#include <set>
#include <stdexcept>

#include <yaml-cpp/yaml.h>

#include <userver/formats/json/value_builder.hpp>

namespace yapi::bodies {

namespace json = userver::formats::json;
using json::ValueBuilder;

namespace {

class JsonReader {
public:
    explicit JsonReader(const std::string& s) : s_(s) {}

    Value Read(std::vector<std::string>& dups) {
        auto v = Parse("", dups);
        Skip();
        if (pos_ != s_.size()) Fail("unexpected data after the value");
        return v;
    }

private:
    [[noreturn]] void Fail(const std::string& what) const {
        throw std::runtime_error(what + " at offset " + std::to_string(pos_));
    }
    void Skip() {
        while (pos_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[pos_]))) ++pos_;
    }
    char Peek() {
        Skip();
        if (pos_ >= s_.size()) Fail("unexpected end of input");
        return s_[pos_];
    }
    void Expect(char c) {
        if (Peek() != c) Fail(std::string("expected '") + c + "'");
        ++pos_;
    }
    static void Utf8(std::string& out, unsigned cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    unsigned Hex4() {
        if (pos_ + 4 > s_.size()) Fail("bad \\u escape");
        unsigned v = std::stoul(s_.substr(pos_, 4), nullptr, 16);
        pos_ += 4;
        return v;
    }
    std::string String() {
        Expect('"');
        std::string out;
        while (pos_ < s_.size() && s_[pos_] != '"') {
            char c = s_[pos_++];
            if (c != '\\') {
                out += c;
                continue;
            }
            if (pos_ >= s_.size()) Fail("bad escape");
            char e = s_[pos_++];
            switch (e) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u': {
                    unsigned cp = Hex4();
                    if (cp >= 0xD800 && cp < 0xDC00 && s_.compare(pos_, 2, "\\u") == 0) {
                        pos_ += 2;
                        unsigned lo = Hex4();
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    Utf8(out, cp);
                    break;
                }
                default: out += e;
            }
        }
        if (pos_ >= s_.size()) Fail("unterminated string");
        ++pos_;
        return out;
    }
    Value Parse(const std::string& path, std::vector<std::string>& dups) {
        char c = Peek();
        if (c == '{') {
            ++pos_;
            ValueBuilder obj(json::Type::kObject);
            std::set<std::string> seen;
            if (Peek() == '}') {
                ++pos_;
                return obj.ExtractValue();
            }
            for (;;) {
                auto key = String();
                Expect(':');
                auto child = path.empty() ? key : path + "." + key;
                if (!seen.insert(key).second) dups.push_back(child);
                obj[key] = Parse(child, dups);
                if (Peek() == ',') {
                    ++pos_;
                    continue;
                }
                Expect('}');
                return obj.ExtractValue();
            }
        }
        if (c == '[') {
            ++pos_;
            ValueBuilder arr(json::Type::kArray);
            if (Peek() == ']') {
                ++pos_;
                return arr.ExtractValue();
            }
            for (std::size_t i = 0;; ++i) {
                arr.PushBack(Parse(path + "[" + std::to_string(i) + "]", dups));
                if (Peek() == ',') {
                    ++pos_;
                    continue;
                }
                Expect(']');
                return arr.ExtractValue();
            }
        }
        if (c == '"') return ValueBuilder(String()).ExtractValue();
        if (s_.compare(pos_, 4, "true") == 0) {
            pos_ += 4;
            return ValueBuilder(true).ExtractValue();
        }
        if (s_.compare(pos_, 5, "false") == 0) {
            pos_ += 5;
            return ValueBuilder(false).ExtractValue();
        }
        if (s_.compare(pos_, 4, "null") == 0) {
            pos_ += 4;
            return Value();
        }
        std::size_t start = pos_;
        bool real = false;
        if (pos_ < s_.size() && s_[pos_] == '-') ++pos_;
        while (pos_ < s_.size() && (std::isdigit(static_cast<unsigned char>(s_[pos_])) || s_[pos_] == '.' || s_[pos_] == 'e' ||
                                    s_[pos_] == 'E' || ((s_[pos_] == '+' || s_[pos_] == '-') && pos_ > start))) {
            real = real || s_[pos_] == '.' || s_[pos_] == 'e' || s_[pos_] == 'E';
            ++pos_;
        }
        if (pos_ == start) Fail("invalid character");
        auto text = s_.substr(start, pos_ - start);
        try {
            if (!real) return ValueBuilder(static_cast<std::int64_t>(std::stoll(text))).ExtractValue();
            return ValueBuilder(std::stod(text)).ExtractValue();
        } catch (const std::exception&) {
            Fail("invalid number");
        }
    }

    const std::string& s_;
    std::size_t pos_ = 0;
};

// A plain YAML scalar the way sigs.k8s.io/yaml reads it into JSON.
Value Scalar(const YAML::Node& n) {
    const auto& text = n.Scalar();
    if (n.Tag() == "!") return ValueBuilder(text).ExtractValue();  // quoted: always a string
    if (text.empty() || text == "~" || text == "null" || text == "Null" || text == "NULL") return Value();
    if (text == "true" || text == "True" || text == "TRUE") return ValueBuilder(true).ExtractValue();
    if (text == "false" || text == "False" || text == "FALSE") return ValueBuilder(false).ExtractValue();
    static const std::regex kInt("^[-+]?(0|[1-9][0-9]*)$");
    static const std::regex kFloat("^[-+]?(\\.[0-9]+|[0-9]+(\\.[0-9]*)?)([eE][-+]?[0-9]+)?$");
    if (std::regex_match(text, kInt)) {
        try {
            return ValueBuilder(static_cast<std::int64_t>(std::stoll(text))).ExtractValue();
        } catch (const std::exception&) {
        }
    }
    if (std::regex_match(text, kFloat)) return ValueBuilder(std::stod(text)).ExtractValue();
    return ValueBuilder(text).ExtractValue();
}

Value Convert(const YAML::Node& n) {
    switch (n.Type()) {
        case YAML::NodeType::Map: {
            ValueBuilder obj(json::Type::kObject);
            for (const auto& kv : n) obj[kv.first.Scalar()] = Convert(kv.second);
            return obj.ExtractValue();
        }
        case YAML::NodeType::Sequence: {
            ValueBuilder arr(json::Type::kArray);
            for (const auto& item : n) arr.PushBack(Convert(item));
            return arr.ExtractValue();
        }
        case YAML::NodeType::Scalar:
            return Scalar(n);
        default:
            return Value();
    }
}

// The first key a block mapping repeats, in go-yaml's words, from the
// lines themselves: yaml-cpp keeps one of the two silently.
std::string YamlDuplicate(const std::string& body) {
    struct Level {
        int indent;
        std::set<std::string> keys;
    };
    std::vector<Level> stack;
    static const std::regex kKey(R"(^(\s*)(- )?([^\s#:][^:#]*?|"[^"]*"|'[^']*'):(\s|$))");
    std::size_t line_no = 0, start = 0;
    while (start <= body.size()) {
        auto end = body.find('\n', start);
        auto line = body.substr(start, end == std::string::npos ? std::string::npos : end - start);
        ++line_no;
        start = end == std::string::npos ? body.size() + 1 : end + 1;
        std::smatch m;
        if (!std::regex_search(line, m, kKey)) continue;
        int indent = static_cast<int>(m[1].length());
        if (m[2].matched) {
            // A list item opens a new mapping.
            indent += 2;
            while (!stack.empty() && stack.back().indent >= indent) stack.pop_back();
            stack.push_back({indent, {}});
        }
        while (!stack.empty() && stack.back().indent > indent) stack.pop_back();
        if (stack.empty() || stack.back().indent < indent) stack.push_back({indent, {}});
        auto key = m[3].str();
        if (key.size() >= 2 && (key.front() == '"' || key.front() == '\'')) key = key.substr(1, key.size() - 2);
        if (!stack.back().keys.insert(key).second) {
            return "error converting YAML to JSON: yaml: unmarshal errors:\n  line " + std::to_string(line_no) + ": key \"" + key + "\" already set in map";
        }
    }
    return "";
}

}  // namespace

Parsed Json(const std::string& body) {
    Parsed p;
    p.value = JsonReader(body).Read(p.duplicate_fields);
    return p;
}

Parsed Yaml(const std::string& body) {
    Parsed p;
    YAML::Node root;
    try {
        root = YAML::Load(body);
    } catch (const YAML::Exception& e) {
        throw std::runtime_error(std::string("error converting YAML to JSON: ") + e.what());
    }
    p.value = Convert(root);
    p.yaml_duplicate = YamlDuplicate(body);
    return p;
}

bool IsYaml(const std::string& content_type) {
    return content_type.find("yaml") != std::string::npos;
}

}  // namespace yapi::bodies
