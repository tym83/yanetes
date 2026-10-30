#include <cel.hpp>

#include <cmath>
#include <limits>
#include <regex>
#include <set>
#include <sstream>

#include <userver/formats/json/serialize.hpp>
#include <userver/formats/json/value_builder.hpp>

namespace yapi::cel {

namespace json = userver::formats::json;
using json::ValueBuilder;
using K = Type::Kind;

// ---------------------------------------------------------------- types

TypeP Type::Dyn() {
    static const TypeP t = std::make_shared<Type>();
    return t;
}

TypeP Type::Of(Kind k) {
    auto t = std::make_shared<Type>();
    t->kind = k;
    return t;
}

TypeP Type::List(TypeP elem) {
    auto t = std::make_shared<Type>();
    t->kind = K::kList;
    t->elem = elem ? elem : Dyn();
    return t;
}

TypeP Type::Map(TypeP value) {
    auto t = std::make_shared<Type>();
    t->kind = K::kMap;
    t->elem = value ? value : Dyn();
    return t;
}

TypeP Type::Object(Value schema, std::string name) {
    auto t = std::make_shared<Type>();
    t->kind = K::kObject;
    t->schema = std::move(schema);
    t->name = std::move(name);
    return t;
}

std::string TypeName(const TypeP& t) {
    switch (t->kind) {
        case K::kDyn: return "dyn";
        case K::kNull: return "null_type";
        case K::kBool: return "bool";
        case K::kInt: return "int";
        case K::kUint: return "uint";
        case K::kDouble: return "double";
        case K::kString: return "string";
        case K::kBytes: return "bytes";
        case K::kList: return "list(" + TypeName(t->elem) + ")";
        case K::kMap: return "map(string, " + TypeName(t->elem) + ")";
        case K::kObject: return t->name.empty() ? "object" : t->name;
    }
    return "dyn";
}

namespace {

Value Resolve(const Value& schema, const std::map<std::string, Value>* components) {
    if (!schema.IsObject() || components == nullptr) return schema;
    auto ref = schema["$ref"].As<std::string>("");
    if (ref.empty() && schema["allOf"].IsArray() && schema["allOf"].GetSize() == 1) ref = schema["allOf"][0]["$ref"].As<std::string>("");
    const std::string prefix = "#/components/schemas/";
    if (ref.starts_with(prefix)) {
        auto it = components->find(ref.substr(prefix.size()));
        if (it != components->end()) return it->second;
    }
    return schema;
}

}  // namespace

TypeP FromSchema(const Value& raw, const std::map<std::string, Value>* components, const std::string& name) {
    const auto s = Resolve(raw, components);
    if (!s.IsObject()) return Type::Dyn();
    if (s["x-kubernetes-int-or-string"].As<bool>(false) || s["format"].As<std::string>("") == "int-or-string") return Type::Dyn();
    auto type = s["type"].As<std::string>("");
    if (type == "integer") return Type::Of(K::kInt);
    if (type == "number") return Type::Of(K::kDouble);
    if (type == "boolean") return Type::Of(K::kBool);
    if (type == "string") return s["format"].As<std::string>("") == "byte" ? Type::Of(K::kBytes) : Type::Of(K::kString);
    if (type == "array") return Type::List(FromSchema(s["items"], components, name + ".@idx"));
    if (s.HasMember("properties")) return Type::Object(s, name);
    if (s["additionalProperties"].IsObject()) return Type::Map(FromSchema(s["additionalProperties"], components, name + ".@elem"));
    return Type::Dyn();
}

// ---------------------------------------------------------------- AST

struct Node {
    enum class Kind { kLit, kIdent, kSelect, kIndex, kCall, kList, kMap, kStruct, kCond, kAnd, kOr, kUnary, kBinary, kHas, kCompr };
    Kind kind;
    std::size_t pos = 0;
    Value lit;
    K lit_kind = K::kNull;
    std::string name;  // identifier, field, function, operator, type, macro
    std::vector<std::shared_ptr<const Node>> args;
    std::vector<std::string> keys;  // struct field names
    bool receiver = false;          // a call on a target: args[0] is it
    std::string var;                // comprehension variable
};
using NodeP = std::shared_ptr<const Node>;

namespace {

std::string Where(const std::string& src, std::size_t pos) {
    std::size_t line = 1, col = 1;
    for (std::size_t i = 0; i < pos && i < src.size(); ++i) {
        if (src[i] == '\n') {
            ++line;
            col = 1;
        } else {
            ++col;
        }
    }
    return "ERROR: <input>:" + std::to_string(line) + ":" + std::to_string(col) + ": ";
}

// ---------------------------------------------------------------- lexer

struct Token {
    enum class T { kInt, kUint, kDouble, kString, kBytes, kIdent, kPunct, kEnd };
    T t;
    std::string text;
    std::size_t pos;
};

std::vector<Token> Lex(const std::string& s) {
    std::vector<Token> out;
    std::size_t i = 0;
    auto fail = [&](const std::string& msg) { throw CompileError(Where(s, i) + "Syntax error: " + msg); };
    while (i < s.size()) {
        char c = s[i];
        if (std::isspace(static_cast<unsigned char>(c))) {
            ++i;
            continue;
        }
        if (c == '/' && i + 1 < s.size() && s[i + 1] == '/') {
            while (i < s.size() && s[i] != '\n') ++i;
            continue;
        }
        const std::size_t start = i;
        bool raw = false, bytes = false;
        if ((c == 'r' || c == 'R' || c == 'b' || c == 'B') && i + 1 < s.size() && (s[i + 1] == '"' || s[i + 1] == '\'')) {
            raw = c == 'r' || c == 'R';
            bytes = c == 'b' || c == 'B';
            ++i;
            c = s[i];
        }
        if (c == '"' || c == '\'') {
            const char q = c;
            const bool triple = s.compare(i, 3, std::string(3, q)) == 0;
            i += triple ? 3 : 1;
            std::string str;
            for (;;) {
                if (i >= s.size()) fail("unterminated string");
                if (triple ? s.compare(i, 3, std::string(3, q)) == 0 : s[i] == q) {
                    i += triple ? 3 : 1;
                    break;
                }
                if (!triple && s[i] == '\n') fail("unterminated string");
                if (s[i] == '\\' && !raw) {
                    ++i;
                    if (i >= s.size()) fail("unterminated string");
                    char e = s[i++];
                    switch (e) {
                        case 'n': str += '\n'; break;
                        case 't': str += '\t'; break;
                        case 'r': str += '\r'; break;
                        case '\\': str += '\\'; break;
                        case '"': str += '"'; break;
                        case '\'': str += '\''; break;
                        case '`': str += '`'; break;
                        case '?': str += '?'; break;
                        default: str += '\\'; str += e;
                    }
                    continue;
                }
                str += s[i++];
            }
            out.push_back({bytes ? Token::T::kBytes : Token::T::kString, str, start});
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(c)) || (c == '.' && i + 1 < s.size() && std::isdigit(static_cast<unsigned char>(s[i + 1])))) {
            bool dbl = false;
            if (c == '0' && i + 1 < s.size() && (s[i + 1] == 'x' || s[i + 1] == 'X')) {
                i += 2;
                while (i < s.size() && std::isxdigit(static_cast<unsigned char>(s[i]))) ++i;
            } else {
                while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
                if (i < s.size() && s[i] == '.' && i + 1 < s.size() && std::isdigit(static_cast<unsigned char>(s[i + 1]))) {
                    dbl = true;
                    ++i;
                    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
                }
                if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
                    dbl = true;
                    ++i;
                    if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
                    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
                }
            }
            auto text = s.substr(start, i - start);
            if (!dbl && i < s.size() && (s[i] == 'u' || s[i] == 'U')) {
                ++i;
                out.push_back({Token::T::kUint, text, start});
            } else {
                out.push_back({dbl ? Token::T::kDouble : Token::T::kInt, text, start});
            }
            continue;
        }
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            while (i < s.size() && (std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '_')) ++i;
            out.push_back({Token::T::kIdent, s.substr(start, i - start), start});
            continue;
        }
        static const std::vector<std::string> kTwo = {"==", "!=", "<=", ">=", "&&", "||"};
        bool two = false;
        for (const auto& p : kTwo) {
            if (s.compare(i, 2, p) == 0) {
                out.push_back({Token::T::kPunct, p, start});
                i += 2;
                two = true;
                break;
            }
        }
        if (two) continue;
        if (std::string("+-*/%<>!?:.,()[]{}").find(c) == std::string::npos) {
            fail("token recognition error at: '" + std::string(1, c) + "'");
        }
        out.push_back({Token::T::kPunct, std::string(1, c), start});
        ++i;
    }
    out.push_back({Token::T::kEnd, "", s.size()});
    return out;
}

// ---------------------------------------------------------------- parser

class Parser {
public:
    Parser(const std::string& src) : src_(src), toks_(Lex(src)) {}

    NodeP Parse() {
        auto e = Expr();
        if (Peek().t != Token::T::kEnd) Fail("extraneous input '" + Peek().text + "' expecting <EOF>");
        return e;
    }

private:
    const Token& Peek(std::size_t ahead = 0) const { return toks_[std::min(i_ + ahead, toks_.size() - 1)]; }
    bool Is(const std::string& p, std::size_t ahead = 0) const {
        return Peek(ahead).t == Token::T::kPunct && Peek(ahead).text == p;
    }
    bool Accept(const std::string& p) {
        if (!Is(p)) return false;
        ++i_;
        return true;
    }
    [[noreturn]] void Fail(const std::string& msg) const {
        throw CompileError(Where(src_, Peek().pos) + "Syntax error: " + msg);
    }
    void Expect(const std::string& p) {
        if (!Accept(p)) {
            Fail(Peek().t == Token::T::kEnd ? "mismatched input '<EOF>' expecting '" + p + "'"
                                            : "mismatched input '" + Peek().text + "' expecting '" + p + "'");
        }
    }
    static NodeP Make(Node n) { return std::make_shared<const Node>(std::move(n)); }

    NodeP Expr() {
        auto cond = Or();
        if (!Is("?")) return cond;
        auto pos = Peek().pos;
        ++i_;
        auto yes = Or();
        Expect(":");
        auto no = Expr();
        Node n{Node::Kind::kCond, pos};
        n.args = {cond, yes, no};
        return Make(std::move(n));
    }
    NodeP Or() {
        auto l = And();
        while (Is("||")) {
            auto pos = Peek().pos;
            ++i_;
            Node n{Node::Kind::kOr, pos};
            n.args = {l, And()};
            l = Make(std::move(n));
        }
        return l;
    }
    NodeP And() {
        auto l = Rel();
        while (Is("&&")) {
            auto pos = Peek().pos;
            ++i_;
            Node n{Node::Kind::kAnd, pos};
            n.args = {l, Rel()};
            l = Make(std::move(n));
        }
        return l;
    }
    NodeP Rel() {
        auto l = Add();
        for (;;) {
            std::string op;
            for (const char* p : {"==", "!=", "<=", ">=", "<", ">"}) {
                if (Is(p)) op = p;
            }
            if (op.empty() && Peek().t == Token::T::kIdent && Peek().text == "in") op = "in";
            if (op.empty()) return l;
            auto pos = Peek().pos;
            ++i_;
            Node n{Node::Kind::kBinary, pos};
            n.name = op;
            n.args = {l, Add()};
            l = Make(std::move(n));
        }
    }
    NodeP Add() {
        auto l = Mul();
        while (Is("+") || Is("-")) {
            auto pos = Peek().pos;
            auto op = Peek().text;
            ++i_;
            Node n{Node::Kind::kBinary, pos};
            n.name = op;
            n.args = {l, Mul()};
            l = Make(std::move(n));
        }
        return l;
    }
    NodeP Mul() {
        auto l = Unary();
        while (Is("*") || Is("/") || Is("%")) {
            auto pos = Peek().pos;
            auto op = Peek().text;
            ++i_;
            Node n{Node::Kind::kBinary, pos};
            n.name = op;
            n.args = {l, Unary()};
            l = Make(std::move(n));
        }
        return l;
    }
    NodeP Unary() {
        if (Is("!") || Is("-")) {
            auto pos = Peek().pos;
            auto op = Peek().text;
            ++i_;
            auto operand = Unary();
            // A negative literal is a literal, as cel-go parses -9223372036854775808.
            if (op == "-" && operand->kind == Node::Kind::kLit && (operand->lit_kind == K::kInt || operand->lit_kind == K::kDouble)) {
                Node n = *operand;
                n.pos = pos;
                n.lit = operand->lit_kind == K::kInt ? ValueBuilder(-operand->lit.As<std::int64_t>()).ExtractValue()
                                                     : ValueBuilder(-operand->lit.As<double>()).ExtractValue();
                return Make(std::move(n));
            }
            Node n{Node::Kind::kUnary, pos};
            n.name = op;
            n.args = {operand};
            return Make(std::move(n));
        }
        return Member();
    }
    std::vector<NodeP> Args(const std::string& close) {
        std::vector<NodeP> out;
        if (Accept(close)) return out;
        for (;;) {
            out.push_back(Expr());
            if (Accept(close)) return out;
            Expect(",");
            if (Accept(close)) return out;  // a trailing comma
        }
    }
    // A qualified name a struct literal may start with: Object.spec
    static bool QualifiedName(const NodeP& n, std::string& out) {
        if (n->kind == Node::Kind::kIdent) {
            out = n->name;
            return true;
        }
        if (n->kind == Node::Kind::kSelect && QualifiedName(n->args[0], out)) {
            out += "." + n->name;
            return true;
        }
        return false;
    }
    NodeP Member() {
        auto e = Primary();
        for (;;) {
            if (Is(".")) {
                auto pos = Peek().pos;
                ++i_;
                if (Peek().t != Token::T::kIdent) Fail("no viable alternative at input '." + Peek().text + "'");
                auto field = Peek().text;
                ++i_;
                if (Is("(")) {
                    ++i_;
                    auto args = Args(")");
                    e = Call(pos, field, e, std::move(args));
                } else {
                    Node n{Node::Kind::kSelect, pos};
                    n.name = field;
                    n.args = {e};
                    e = Make(std::move(n));
                }
            } else if (Is("[")) {
                auto pos = Peek().pos;
                ++i_;
                auto idx = Expr();
                Expect("]");
                Node n{Node::Kind::kIndex, pos};
                n.args = {e, idx};
                e = Make(std::move(n));
            } else if (Is("{")) {
                std::string type;
                if (!QualifiedName(e, type)) return e;
                auto pos = Peek().pos;
                ++i_;
                Node n{Node::Kind::kStruct, pos};
                n.name = type;
                if (!Accept("}")) {
                    for (;;) {
                        if (Peek().t != Token::T::kIdent) Fail("mismatched input '" + Peek().text + "' expecting IDENTIFIER");
                        n.keys.push_back(Peek().text);
                        ++i_;
                        Expect(":");
                        n.args.push_back(Expr());
                        if (Accept("}")) break;
                        Expect(",");
                        if (Accept("}")) break;
                    }
                }
                e = Make(std::move(n));
            } else {
                return e;
            }
        }
    }
    NodeP Call(std::size_t pos, const std::string& fn, NodeP target, std::vector<NodeP> args) {
        static const std::set<std::string> kMacros = {"all", "exists", "exists_one", "map", "filter"};
        if (target && kMacros.count(fn) && (args.size() == 2 || (fn == "map" && args.size() == 3))) {
            if (args[0]->kind != Node::Kind::kIdent) throw CompileError(Where(src_, pos) + "argument must be a simple name");
            Node n{Node::Kind::kCompr, pos};
            n.name = fn;
            n.var = args[0]->name;
            n.args = {target};
            for (std::size_t k = 1; k < args.size(); ++k) n.args.push_back(args[k]);
            return Make(std::move(n));
        }
        if (!target && fn == "has") {
            if (args.size() != 1 || args[0]->kind != Node::Kind::kSelect) {
                throw CompileError(Where(src_, pos) + "invalid argument to has() macro");
            }
            Node n{Node::Kind::kHas, pos};
            n.args = {args[0]};
            return Make(std::move(n));
        }
        Node n{Node::Kind::kCall, pos};
        n.name = fn;
        n.receiver = target != nullptr;
        if (target) n.args.push_back(target);
        for (auto& a : args) n.args.push_back(std::move(a));
        return Make(std::move(n));
    }
    NodeP Primary() {
        const auto tok = Peek();
        switch (tok.t) {
            case Token::T::kInt:
            case Token::T::kUint: {
                ++i_;
                Node n{Node::Kind::kLit, tok.pos};
                try {
                    n.lit = tok.t == Token::T::kInt ? ValueBuilder(static_cast<std::int64_t>(std::stoll(tok.text, nullptr, 0))).ExtractValue()
                                                    : ValueBuilder(static_cast<std::uint64_t>(std::stoull(tok.text, nullptr, 0))).ExtractValue();
                } catch (const std::exception&) {
                    throw CompileError(Where(src_, tok.pos) + "invalid int literal");
                }
                n.lit_kind = tok.t == Token::T::kInt ? K::kInt : K::kUint;
                return Make(std::move(n));
            }
            case Token::T::kDouble: {
                ++i_;
                Node n{Node::Kind::kLit, tok.pos};
                n.lit = ValueBuilder(std::stod(tok.text)).ExtractValue();
                n.lit_kind = K::kDouble;
                return Make(std::move(n));
            }
            case Token::T::kString:
            case Token::T::kBytes: {
                ++i_;
                Node n{Node::Kind::kLit, tok.pos};
                n.lit = ValueBuilder(tok.text).ExtractValue();
                n.lit_kind = tok.t == Token::T::kString ? K::kString : K::kBytes;
                return Make(std::move(n));
            }
            case Token::T::kIdent: {
                ++i_;
                if (tok.text == "true" || tok.text == "false") {
                    Node n{Node::Kind::kLit, tok.pos};
                    n.lit = ValueBuilder(tok.text == "true").ExtractValue();
                    n.lit_kind = K::kBool;
                    return Make(std::move(n));
                }
                if (tok.text == "null") {
                    Node n{Node::Kind::kLit, tok.pos};
                    n.lit_kind = K::kNull;
                    return Make(std::move(n));
                }
                if (Is("(")) {
                    ++i_;
                    return Call(tok.pos, tok.text, nullptr, Args(")"));
                }
                Node n{Node::Kind::kIdent, tok.pos};
                n.name = tok.text;
                return Make(std::move(n));
            }
            case Token::T::kPunct: {
                if (tok.text == "(") {
                    ++i_;
                    auto e = Expr();
                    Expect(")");
                    return e;
                }
                if (tok.text == "[") {
                    ++i_;
                    Node n{Node::Kind::kList, tok.pos};
                    n.args = Args("]");
                    return Make(std::move(n));
                }
                if (tok.text == "{") {
                    ++i_;
                    Node n{Node::Kind::kMap, tok.pos};
                    if (!Accept("}")) {
                        for (;;) {
                            n.args.push_back(Expr());
                            Expect(":");
                            n.args.push_back(Expr());
                            if (Accept("}")) break;
                            Expect(",");
                            if (Accept("}")) break;
                        }
                    }
                    return Make(std::move(n));
                }
                if (tok.text == ".") {
                    ++i_;
                    return Primary();
                }
                Fail("mismatched input '" + tok.text + "' expecting {'[', '{', '(', '.', '-', '!', 'true', 'false', 'null', NUM_FLOAT, NUM_INT, NUM_UINT, STRING, BYTES, IDENTIFIER}");
            }
            case Token::T::kEnd:
                Fail("mismatched input '<EOF>' expecting {'[', '{', '(', '.', '-', '!', 'true', 'false', 'null', NUM_FLOAT, NUM_INT, NUM_UINT, STRING, BYTES, IDENTIFIER}");
        }
        Fail("unexpected input");
    }

    const std::string& src_;
    std::vector<Token> toks_;
    std::size_t i_ = 0;
};

// ---------------------------------------------------------------- checker

bool Numeric(K k) { return k == K::kInt || k == K::kUint || k == K::kDouble; }

struct Checker {
    const std::string& src;
    const Env& env;
    std::vector<std::string> errors;
    std::map<std::string, TypeP> scope;

    void Error(std::size_t pos, const std::string& msg) { errors.push_back(Where(src, pos) + msg); }

    TypeP Field(std::size_t pos, const TypeP& t, const std::string& field) {
        if (t->kind == K::kMap) return t->elem;
        if (t->kind != K::kObject) return Type::Dyn();
        const auto props = t->schema["properties"];
        if (props.IsObject() && props.HasMember(field)) return FromSchema(props[field], env.components, t->name + "." + field);
        // Every object has metadata, apiVersion and kind, even where a
        // custom resource's schema leaves them out.
        if (field == "metadata" || field == "apiVersion" || field == "kind") return Type::Dyn();
        if (t->schema["x-kubernetes-preserve-unknown-fields"].As<bool>(false)) return Type::Dyn();
        Error(pos, "undefined field '" + field + "'");
        return Type::Dyn();
    }

    void NoOverload(std::size_t pos, const std::string& op, const std::vector<TypeP>& ts) {
        std::string list;
        for (std::size_t i = 0; i < ts.size(); ++i) list += (i ? ", " : "") + TypeName(ts[i]);
        Error(pos, "found no matching overload for '" + op + "' applied to '(" + list + ")'");
    }

    TypeP Check(const NodeP& n) {
        using NK = Node::Kind;
        switch (n->kind) {
            case NK::kLit:
                return Type::Of(n->lit_kind);
            case NK::kIdent: {
                if (auto it = scope.find(n->name); it != scope.end()) return it->second;
                if (auto it = env.vars.find(n->name); it != env.vars.end()) return it->second;
                Error(n->pos, "undeclared reference to '" + n->name + "' (in container '')");
                return Type::Dyn();
            }
            case NK::kSelect:
                return Field(n->pos, Check(n->args[0]), n->name);
            case NK::kHas:
                Check(n->args[0]);
                return Type::Of(K::kBool);
            case NK::kIndex: {
                auto t = Check(n->args[0]);
                Check(n->args[1]);
                if (t->kind == K::kList || t->kind == K::kMap) return t->elem;
                return Type::Dyn();
            }
            case NK::kList: {
                TypeP elem;
                for (const auto& a : n->args) {
                    auto t = Check(a);
                    if (!elem) elem = t;
                    else if (elem->kind != t->kind) elem = Type::Dyn();
                }
                return Type::List(elem);
            }
            case NK::kMap: {
                TypeP elem;
                for (std::size_t i = 0; i < n->args.size(); i += 2) {
                    Check(n->args[i]);
                    auto t = Check(n->args[i + 1]);
                    if (!elem) elem = t;
                    else if (elem->kind != t->kind) elem = Type::Dyn();
                }
                return Type::Map(elem);
            }
            case NK::kStruct:
                for (const auto& a : n->args) Check(a);
                return Type::Dyn();
            case NK::kCond: {
                auto c = Check(n->args[0]);
                if (c->kind != K::kBool && c->kind != K::kDyn) NoOverload(n->pos, "_?_:_", {c});
                auto a = Check(n->args[1]);
                auto b = Check(n->args[2]);
                return a->kind == b->kind ? a : Type::Dyn();
            }
            case NK::kAnd:
            case NK::kOr: {
                auto a = Check(n->args[0]);
                auto b = Check(n->args[1]);
                for (const auto& t : {a, b}) {
                    if (t->kind != K::kBool && t->kind != K::kDyn) NoOverload(n->pos, n->kind == NK::kAnd ? "_&&_" : "_||_", {a, b});
                }
                return Type::Of(K::kBool);
            }
            case NK::kUnary: {
                auto t = Check(n->args[0]);
                if (n->name == "!") {
                    if (t->kind != K::kBool && t->kind != K::kDyn) NoOverload(n->pos, "!_", {t});
                    return Type::Of(K::kBool);
                }
                if (!Numeric(t->kind) && t->kind != K::kDyn) NoOverload(n->pos, "-_", {t});
                return t;
            }
            case NK::kBinary: {
                auto a = Check(n->args[0]);
                auto b = Check(n->args[1]);
                const auto& op = n->name;
                const bool dyn = a->kind == K::kDyn || b->kind == K::kDyn;
                const std::string fn = op == "in" ? "@in" : "_" + op + "_";
                if (op == "in") {
                    if (b->kind != K::kList && b->kind != K::kMap && b->kind != K::kDyn) NoOverload(n->pos, fn, {a, b});
                    return Type::Of(K::kBool);
                }
                if (op == "==" || op == "!=") {
                    bool ok = dyn || a->kind == b->kind || (Numeric(a->kind) && Numeric(b->kind)) || a->kind == K::kNull || b->kind == K::kNull ||
                              ((a->kind == K::kObject || a->kind == K::kMap) && (b->kind == K::kObject || b->kind == K::kMap));
                    if (!ok) NoOverload(n->pos, fn, {a, b});
                    return Type::Of(K::kBool);
                }
                if (op == "<" || op == "<=" || op == ">" || op == ">=") {
                    bool ok = dyn || (Numeric(a->kind) && Numeric(b->kind)) ||
                              (a->kind == b->kind && (a->kind == K::kString || a->kind == K::kBool || a->kind == K::kBytes));
                    if (!ok) NoOverload(n->pos, fn, {a, b});
                    return Type::Of(K::kBool);
                }
                if (dyn) return Type::Dyn();
                if (op == "+") {
                    if (a->kind == b->kind && (Numeric(a->kind) || a->kind == K::kString || a->kind == K::kBytes || a->kind == K::kList)) return a;
                } else if (a->kind == b->kind && Numeric(a->kind) && !(op == "%" && a->kind == K::kDouble)) {
                    return a;
                }
                NoOverload(n->pos, fn, {a, b});
                return Type::Dyn();
            }
            case NK::kCall: {
                std::vector<TypeP> ts;
                for (const auto& a : n->args) ts.push_back(Check(a));
                const auto& f = n->name;
                if (f == "size") return Type::Of(K::kInt);
                if (f == "contains" || f == "startsWith" || f == "endsWith" || f == "matches") return Type::Of(K::kBool);
                if (f == "int") return Type::Of(K::kInt);
                if (f == "uint") return Type::Of(K::kUint);
                if (f == "double") return Type::Of(K::kDouble);
                if (f == "string" || f == "lowerAscii" || f == "upperAscii" || f == "trim" || f == "join" || f == "replace" || f == "format") return Type::Of(K::kString);
                if (f == "bool") return Type::Of(K::kBool);
                if (f == "indexOf" || f == "lastIndexOf") return Type::Of(K::kInt);
                if (f == "split") return Type::List(Type::Of(K::kString));
                return Type::Dyn();
            }
            case NK::kCompr: {
                auto range = Check(n->args[0]);
                auto saved = scope;
                scope[n->var] = range->kind == K::kList ? range->elem : range->kind == K::kMap ? Type::Of(K::kString) : Type::Dyn();
                TypeP last;
                for (std::size_t i = 1; i < n->args.size(); ++i) last = Check(n->args[i]);
                scope = saved;
                if (n->name == "map") return Type::List(last);
                if (n->name == "filter") return range;
                return Type::Of(K::kBool);
            }
        }
        return Type::Dyn();
    }
};

// ---------------------------------------------------------------- evaluation

enum class R { kNull, kBool, kInt, kDouble, kString, kList, kMap };

R Kind(const Value& v) {
    if (v.IsNull() || v.IsMissing()) return R::kNull;
    if (v.IsBool()) return R::kBool;
    if (v.IsString()) return R::kString;
    if (v.IsArray()) return R::kList;
    if (v.IsObject()) return R::kMap;
    if (v.IsDouble() && !v.IsInt64()) return R::kDouble;
    return R::kInt;
}

const char* KindName(R k) {
    switch (k) {
        case R::kNull: return "null_type";
        case R::kBool: return "bool";
        case R::kInt: return "int";
        case R::kDouble: return "double";
        case R::kString: return "string";
        case R::kList: return "list";
        case R::kMap: return "map";
    }
    return "dyn";
}

[[noreturn]] void NoSuchOverload(const std::string& op, const Value& a, const Value& b) {
    throw EvalError(std::string("no such overload: ") + KindName(Kind(a)) + " " + op + " " + KindName(Kind(b)));
}

Value V(std::int64_t x) { return ValueBuilder(x).ExtractValue(); }
Value V(double x) { return ValueBuilder(x).ExtractValue(); }
Value V(bool x) { return ValueBuilder(x).ExtractValue(); }
Value V(const std::string& x) { return ValueBuilder(x).ExtractValue(); }

bool Equal(const Value& a, const Value& b) {
    auto ka = Kind(a), kb = Kind(b);
    if ((ka == R::kInt || ka == R::kDouble) && (kb == R::kInt || kb == R::kDouble)) {
        if (ka == R::kInt && kb == R::kInt) return a.As<std::int64_t>() == b.As<std::int64_t>();
        return a.As<double>() == b.As<double>();
    }
    if (ka != kb) return false;
    if (ka == R::kList) {
        if (a.GetSize() != b.GetSize()) return false;
        for (std::size_t i = 0; i < a.GetSize(); ++i) {
            if (!Equal(a[i], b[i])) return false;
        }
        return true;
    }
    if (ka == R::kMap) {
        if (a.GetSize() != b.GetSize()) return false;
        for (auto it = a.begin(); it != a.end(); ++it) {
            if (!b.HasMember(it.GetName()) || !Equal(*it, b[it.GetName()])) return false;
        }
        return true;
    }
    return a == b;
}

int Compare(const Value& a, const Value& b, const std::string& op) {
    auto ka = Kind(a), kb = Kind(b);
    if ((ka == R::kInt || ka == R::kDouble) && (kb == R::kInt || kb == R::kDouble)) {
        if (ka == R::kInt && kb == R::kInt) {
            auto x = a.As<std::int64_t>(), y = b.As<std::int64_t>();
            return x < y ? -1 : x > y ? 1 : 0;
        }
        auto x = a.As<double>(), y = b.As<double>();
        return x < y ? -1 : x > y ? 1 : 0;
    }
    if (ka == kb && ka == R::kString) {
        auto x = a.As<std::string>(), y = b.As<std::string>();
        return x < y ? -1 : x > y ? 1 : 0;
    }
    if (ka == kb && ka == R::kBool) return static_cast<int>(a.As<bool>()) - static_cast<int>(b.As<bool>());
    NoSuchOverload(op, a, b);
}

struct Evaluator {
    std::map<std::string, Value> vars;

    Value Eval(const NodeP& n) {
        using NK = Node::Kind;
        switch (n->kind) {
            case NK::kLit:
                return n->lit;
            case NK::kIdent: {
                auto it = vars.find(n->name);
                if (it == vars.end()) throw EvalError("no such attribute: " + n->name);
                return it->second;
            }
            case NK::kSelect: {
                auto obj = Eval(n->args[0]);
                if (!obj.IsObject()) throw EvalError("no such key: " + n->name);
                if (!obj.HasMember(n->name)) throw EvalError("no such key: " + n->name);
                return obj[n->name];
            }
            case NK::kHas: {
                const auto& sel = n->args[0];
                auto obj = Eval(sel->args[0]);
                return V(obj.IsObject() && obj.HasMember(sel->name) && !obj[sel->name].IsNull());
            }
            case NK::kIndex: {
                auto obj = Eval(n->args[0]);
                auto idx = Eval(n->args[1]);
                if (obj.IsArray()) {
                    if (Kind(idx) != R::kInt && Kind(idx) != R::kDouble) throw EvalError("no such overload: list[" + std::string(KindName(Kind(idx))) + "]");
                    auto i = idx.As<std::int64_t>();
                    if (i < 0 || static_cast<std::size_t>(i) >= obj.GetSize()) throw EvalError("index out of bounds: " + std::to_string(i));
                    return obj[static_cast<std::size_t>(i)];
                }
                if (obj.IsObject()) {
                    auto key = idx.IsString() ? idx.As<std::string>() : json::ToString(idx);
                    if (!obj.HasMember(key)) throw EvalError("no such key: " + key);
                    return obj[key];
                }
                throw EvalError("no such overload: index on " + std::string(KindName(Kind(obj))));
            }
            case NK::kList: {
                ValueBuilder b(json::Type::kArray);
                for (const auto& a : n->args) b.PushBack(Eval(a));
                return b.ExtractValue();
            }
            case NK::kMap: {
                ValueBuilder b(json::Type::kObject);
                for (std::size_t i = 0; i < n->args.size(); i += 2) {
                    auto k = Eval(n->args[i]);
                    b[k.IsString() ? k.As<std::string>() : json::ToString(k)] = Eval(n->args[i + 1]);
                }
                return b.ExtractValue();
            }
            case NK::kStruct: {
                ValueBuilder b(json::Type::kObject);
                for (std::size_t i = 0; i < n->keys.size(); ++i) b[n->keys[i]] = Eval(n->args[i]);
                return b.ExtractValue();
            }
            case NK::kCond: {
                auto c = Eval(n->args[0]);
                if (!c.IsBool()) throw EvalError("no such overload: conditional on " + std::string(KindName(Kind(c))));
                return Eval(n->args[c.As<bool>() ? 1 : 2]);
            }
            case NK::kAnd:
            case NK::kOr: {
                // Errors are absorbed by a result the other side decides, as CEL does.
                const bool is_and = n->kind == NK::kAnd;
                std::exception_ptr err;
                for (const auto& a : n->args) {
                    try {
                        auto v = Eval(a);
                        if (!v.IsBool()) throw EvalError("no such overload: logical operator on " + std::string(KindName(Kind(v))));
                        if (v.As<bool>() != is_and) return V(!is_and);
                    } catch (const EvalError&) {
                        err = std::current_exception();
                    }
                }
                if (err) std::rethrow_exception(err);
                return V(is_and);
            }
            case NK::kUnary: {
                auto v = Eval(n->args[0]);
                if (n->name == "!") {
                    if (!v.IsBool()) throw EvalError("no such overload: !" + std::string(KindName(Kind(v))));
                    return V(!v.As<bool>());
                }
                if (Kind(v) == R::kInt) return V(-v.As<std::int64_t>());
                if (Kind(v) == R::kDouble) return V(-v.As<double>());
                throw EvalError("no such overload: -" + std::string(KindName(Kind(v))));
            }
            case NK::kBinary:
                return Binary(n);
            case NK::kCall:
                return Call(n);
            case NK::kCompr:
                return Compr(n);
        }
        throw EvalError("unknown expression");
    }

    Value Binary(const NodeP& n) {
        auto a = Eval(n->args[0]);
        auto b = Eval(n->args[1]);
        const auto& op = n->name;
        if (op == "==") return V(Equal(a, b));
        if (op == "!=") return V(!Equal(a, b));
        if (op == "<") return V(Compare(a, b, op) < 0);
        if (op == "<=") return V(Compare(a, b, op) <= 0);
        if (op == ">") return V(Compare(a, b, op) > 0);
        if (op == ">=") return V(Compare(a, b, op) >= 0);
        if (op == "in") {
            if (b.IsArray()) {
                for (const auto& x : b) {
                    if (Equal(a, x)) return V(true);
                }
                return V(false);
            }
            if (b.IsObject()) return V(a.IsString() && b.HasMember(a.As<std::string>()));
            NoSuchOverload("in", a, b);
        }
        auto ka = Kind(a), kb = Kind(b);
        if (op == "+") {
            if (ka == R::kString && kb == R::kString) return V(a.As<std::string>() + b.As<std::string>());
            if (ka == R::kList && kb == R::kList) {
                ValueBuilder out(json::Type::kArray);
                for (const auto& x : a) out.PushBack(x);
                for (const auto& x : b) out.PushBack(x);
                return out.ExtractValue();
            }
        }
        if (ka == R::kInt && kb == R::kInt) {
            auto x = a.As<std::int64_t>(), y = b.As<std::int64_t>();
            std::int64_t r = 0;
            if (op == "+" && !__builtin_add_overflow(x, y, &r)) return V(r);
            if (op == "-" && !__builtin_sub_overflow(x, y, &r)) return V(r);
            if (op == "*" && !__builtin_mul_overflow(x, y, &r)) return V(r);
            if (op == "/") {
                if (y == 0) throw EvalError("division by zero");
                return V(x / y);
            }
            if (op == "%") {
                if (y == 0) throw EvalError("modulus by zero");
                return V(x % y);
            }
            throw EvalError("integer overflow");
        }
        if (ka == R::kDouble && kb == R::kDouble) {
            auto x = a.As<double>(), y = b.As<double>();
            if (op == "+") return V(x + y);
            if (op == "-") return V(x - y);
            if (op == "*") return V(x * y);
            if (op == "/") return V(x / y);
        }
        NoSuchOverload(op, a, b);
    }

    Value Call(const NodeP& n) {
        std::vector<Value> args;
        for (const auto& a : n->args) args.push_back(Eval(a));
        const auto& f = n->name;
        auto str = [&](std::size_t i) {
            if (i >= args.size() || !args[i].IsString()) throw EvalError("no such overload: " + f);
            return args[i].As<std::string>();
        };
        if (f == "size" && args.size() == 1) {
            if (args[0].IsString()) {
                auto s = args[0].As<std::string>();
                std::int64_t count = 0;
                for (unsigned char c : s) count += (c & 0xC0) != 0x80;  // code points, not bytes
                return V(count);
            }
            if (args[0].IsArray() || args[0].IsObject()) return V(static_cast<std::int64_t>(args[0].GetSize()));
            throw EvalError("no such overload: size");
        }
        if (f == "contains") return V(str(0).find(str(1)) != std::string::npos);
        if (f == "startsWith") return V(str(0).starts_with(str(1)));
        if (f == "endsWith") return V(str(0).ends_with(str(1)));
        if (f == "matches") {
            try {
                return V(std::regex_search(str(0), std::regex(str(1), std::regex::ECMAScript)));
            } catch (const std::regex_error&) {
                throw EvalError("invalid regular expression: " + str(1));
            }
        }
        if (f == "lowerAscii" || f == "upperAscii") {
            auto s = str(0);
            for (auto& c : s) c = static_cast<char>(f == "lowerAscii" ? std::tolower(static_cast<unsigned char>(c)) : std::toupper(static_cast<unsigned char>(c)));
            return V(s);
        }
        if (f == "trim") {
            auto s = str(0);
            auto b = s.find_first_not_of(" \t\n\r");
            auto e = s.find_last_not_of(" \t\n\r");
            return V(b == std::string::npos ? std::string() : s.substr(b, e - b + 1));
        }
        if (f == "split" && args.size() == 2) {
            auto s = str(0), sep = str(1);
            ValueBuilder out(json::Type::kArray);
            std::size_t start = 0, pos;
            while (!sep.empty() && (pos = s.find(sep, start)) != std::string::npos) {
                out.PushBack(s.substr(start, pos - start));
                start = pos + sep.size();
            }
            out.PushBack(s.substr(start));
            return out.ExtractValue();
        }
        if (f == "join" && !args.empty() && args[0].IsArray()) {
            std::string sep = args.size() > 1 ? str(1) : "";
            std::string out;
            for (std::size_t i = 0; i < args[0].GetSize(); ++i) out += (i ? sep : "") + args[0][i].As<std::string>("");
            return V(out);
        }
        if (f == "indexOf") {
            auto p = str(0).find(str(1));
            return V(p == std::string::npos ? std::int64_t{-1} : static_cast<std::int64_t>(p));
        }
        if (f == "int" && args.size() == 1) {
            if (Kind(args[0]) == R::kInt) return args[0];
            if (Kind(args[0]) == R::kDouble) return V(static_cast<std::int64_t>(args[0].As<double>()));
            if (args[0].IsString()) {
                try {
                    return V(static_cast<std::int64_t>(std::stoll(args[0].As<std::string>())));
                } catch (const std::exception&) {
                    throw EvalError("cannot convert to int: " + args[0].As<std::string>());
                }
            }
        }
        if (f == "double" && args.size() == 1) {
            if (Kind(args[0]) == R::kInt || Kind(args[0]) == R::kDouble) return V(args[0].As<double>());
            if (args[0].IsString()) return V(std::stod(args[0].As<std::string>()));
        }
        if (f == "string" && args.size() == 1) {
            if (args[0].IsString()) return args[0];
            if (args[0].IsBool()) return V(std::string(args[0].As<bool>() ? "true" : "false"));
            if (Kind(args[0]) == R::kInt) return V(std::to_string(args[0].As<std::int64_t>()));
            if (Kind(args[0]) == R::kDouble) {
                std::ostringstream ss;
                ss << args[0].As<double>();
                return V(ss.str());
            }
        }
        if (f == "dyn" && args.size() == 1) return args[0];
        if (f == "bool" && args.size() == 1 && args[0].IsBool()) return args[0];
        throw EvalError("no such overload: " + f);
    }

    Value Compr(const NodeP& n) {
        auto range = Eval(n->args[0]);
        std::vector<Value> items;
        if (range.IsArray()) {
            for (const auto& x : range) items.push_back(x);
        } else if (range.IsObject()) {
            for (auto it = range.begin(); it != range.end(); ++it) items.push_back(V(it.GetName()));
        } else {
            throw EvalError("no such overload: " + n->name + " on " + KindName(Kind(range)));
        }
        auto saved = vars.count(n->var) ? std::optional<Value>(vars[n->var]) : std::nullopt;
        auto body = [&](const NodeP& e, const Value& x) {
            vars[n->var] = x;
            return Eval(e);
        };
        Value result;
        const auto& m = n->name;
        if (m == "all" || m == "exists" || m == "exists_one") {
            std::int64_t hits = 0;
            std::exception_ptr err;
            bool decided = false;
            for (const auto& x : items) {
                try {
                    auto v = body(n->args[1], x);
                    if (!v.IsBool()) throw EvalError("no such overload: predicate is " + std::string(KindName(Kind(v))));
                    if (m == "all" && !v.As<bool>()) {
                        decided = true;
                        result = V(false);
                        break;
                    }
                    if (m == "exists" && v.As<bool>()) {
                        decided = true;
                        result = V(true);
                        break;
                    }
                    hits += v.As<bool>();
                } catch (const EvalError&) {
                    err = std::current_exception();
                }
            }
            if (!decided) {
                if (err) std::rethrow_exception(err);
                result = m == "all" ? V(true) : m == "exists" ? V(false) : V(hits == 1);
            }
        } else if (m == "filter") {
            ValueBuilder out(json::Type::kArray);
            for (const auto& x : items) {
                auto v = body(n->args[1], x);
                if (v.IsBool() && v.As<bool>()) out.PushBack(x);
            }
            result = out.ExtractValue();
        } else {  // map, with an optional filter in the middle
            ValueBuilder out(json::Type::kArray);
            for (const auto& x : items) {
                if (n->args.size() == 3) {
                    auto keep = body(n->args[1], x);
                    if (!keep.IsBool() || !keep.As<bool>()) continue;
                }
                out.PushBack(body(n->args.back(), x));
            }
            result = out.ExtractValue();
        }
        if (saved) vars[n->var] = *saved;
        else vars.erase(n->var);
        return result;
    }
};

}  // namespace

Program Program::Parse(std::string_view source) {
    Program p;
    p.source_ = std::string(source);
    p.root_ = Parser(p.source_).Parse();
    return p;
}

std::vector<std::string> Program::Check(const Env& env) const {
    Checker c{source_, env, {}, {}};
    c.Check(root_);
    return c.errors;
}

TypeP Program::ResultType(const Env& env) const {
    Checker c{source_, env, {}, {}};
    return c.Check(root_);
}

Value Program::Eval(const Activation& activation) const {
    Evaluator e{activation.vars};
    return e.Eval(root_);
}

}  // namespace yapi::cel
