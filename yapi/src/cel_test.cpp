#include <cel.hpp>

#include <userver/formats/json/serialize.hpp>
#include <userver/utest/utest.hpp>

namespace json = userver::formats::json;
using yapi::cel::Program;

namespace {

yapi::cel::Value Eval(const std::string& expr, const std::string& object = "{}") {
    yapi::cel::Activation a;
    a.vars["object"] = json::FromString(object);
    return Program::Parse(expr).Eval(a);
}

yapi::cel::Env DeploymentEnv() {
    yapi::cel::Env env;
    auto schema = json::FromString(R"({"type":"object","properties":{
        "spec":{"type":"object","properties":{"replicas":{"type":"integer"}}},
        "metadata":{"type":"object","properties":{"name":{"type":"string"}}}}})");
    env.vars["object"] = yapi::cel::FromSchema(schema, nullptr, "Object");
    return env;
}

}  // namespace

TEST(Cel, Evaluates) {
    EXPECT_TRUE(Eval("object.spec.replicas > 1", R"({"spec":{"replicas":3}})").As<bool>());
    EXPECT_FALSE(Eval("object.metadata.name != 'skip-me'", R"({"metadata":{"name":"skip-me"}})").As<bool>());
    EXPECT_EQ(Eval("3 % 2 == 1 && 'a' + 'b' == 'ab' && 1 == 1.0").As<bool>(), true);
    EXPECT_EQ(json::ToString(Eval("Object{spec: Object.spec{replicas: 1337}}")), R"({"spec":{"replicas":1337}})");
    EXPECT_EQ(json::ToString(Eval(R"(Object{metadata: Object.metadata{annotations: {"mutated-by": "x"}}})")),
              R"({"metadata":{"annotations":{"mutated-by":"x"}}})");
    EXPECT_TRUE(Eval("[1, 2, 3].all(x, x > 0) && [1, 2].exists(x, x == 2) && has(object.spec)", R"({"spec":{}})").As<bool>());
    EXPECT_EQ(json::ToString(Eval("[1, 2, 3].map(x, x * 2)")), "[2,4,6]");
    EXPECT_THROW(Eval("'got ' + object.spec.replicas", R"({"spec":{"replicas":3}})"), yapi::cel::EvalError);
}

TEST(Cel, ChecksTypes) {
    auto env = DeploymentEnv();
    EXPECT_TRUE(Program::Parse("object.spec.replicas > 1").Check(env).empty());
    auto errs = Program::Parse("object.spec.replicas > '1'").Check(env);
    ASSERT_EQ(errs.size(), 1u);
    EXPECT_NE(errs[0].find("found no matching overload for '_>_' applied to '(int, string)'"), std::string::npos);
    errs = Program::Parse("'wants replicas > 1, got ' + object.spec.replicas").Check(env);
    ASSERT_EQ(errs.size(), 1u);
    EXPECT_NE(errs[0].find("found no matching overload for '_+_' applied to '(string, int)'"), std::string::npos);
    errs = Program::Parse("object.spec.maxRetries < 10").Check(env);
    ASSERT_EQ(errs.size(), 1u);
    EXPECT_NE(errs[0].find("undefined field 'maxRetries'"), std::string::npos);
}

TEST(Cel, RejectsSyntax) { EXPECT_THROW(Program::Parse("... [] bad expression"), yapi::cel::CompileError); }
