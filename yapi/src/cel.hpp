#pragma once

#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <userver/formats/json/value.hpp>

namespace yapi::cel {

using Value = userver::formats::json::Value;

// A static type, as the checker sees it. Objects and maps carry the
// OpenAPI schema they came from, so a field that is not there is an error.
struct Type;
using TypeP = std::shared_ptr<const Type>;
struct Type {
    enum class Kind { kDyn, kNull, kBool, kInt, kUint, kDouble, kString, kBytes, kList, kMap, kObject };
    Kind kind = Kind::kDyn;
    TypeP elem;   // list element, map value
    Value schema; // object
    std::string name;

    static TypeP Dyn();
    static TypeP Of(Kind k);
    static TypeP List(TypeP elem);
    static TypeP Map(TypeP value);
    static TypeP Object(Value schema, std::string name);
};

std::string TypeName(const TypeP& t);

// A compile error: syntax or types, worded as cel-go words them.
class CompileError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// An error while evaluating: no such key, no such overload, ...
class EvalError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct Node;

// The declarations an expression is checked against.
struct Env {
    std::map<std::string, TypeP> vars;
    // Components of OpenAPI documents, for "$ref"s in schemas.
    const std::map<std::string, Value>* components = nullptr;
};

// Variables an expression is evaluated with.
struct Activation {
    std::map<std::string, Value> vars;
};

class Program {
public:
    // Parses; throws CompileError on a syntax error.
    static Program Parse(std::string_view source);

    // Type problems against the declarations, each a cel-go style message;
    // empty when the expression checks.
    std::vector<std::string> Check(const Env& env) const;
    // The type of the expression, dyn when the checker cannot tell.
    TypeP ResultType(const Env& env) const;

    Value Eval(const Activation& activation) const;

    const std::string& Source() const { return source_; }

private:
    std::string source_;
    std::shared_ptr<const Node> root_;
};

// A schema as a CEL type: integer is int, string is string, objects keep
// their schema; $refs resolve against the components.
TypeP FromSchema(const Value& schema, const std::map<std::string, Value>* components, const std::string& name);

}  // namespace yapi::cel
