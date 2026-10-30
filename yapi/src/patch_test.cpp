#include <patch.hpp>

#include <userver/formats/json/serialize.hpp>
#include <userver/utest/utest.hpp>

#include <status.hpp>

namespace json = userver::formats::json;

TEST(Patch, Merge) {
    auto out = yapi::patch::Merge(json::FromString(R"({"a":1,"b":{"c":2,"d":3}})"), json::FromString(R"({"a":null,"b":{"c":5}})"));
    EXPECT_EQ(out, json::FromString(R"({"b":{"c":5,"d":3}})"));
}

TEST(Patch, JsonPatch) {
    auto doc = json::FromString(R"({"a":[1,2],"b":"x"})");
    auto ops = json::FromString(R"([{"op":"add","path":"/a/1","value":9},{"op":"replace","path":"/b","value":"y"},{"op":"remove","path":"/a/0"},{"op":"test","path":"/b","value":"y"}])");
    EXPECT_EQ(yapi::patch::Json(doc, ops), json::FromString(R"({"a":[9,2],"b":"y"})"));
    auto bad = json::FromString(R"([{"op":"test","path":"/b","value":"z"}])");
    EXPECT_THROW(yapi::patch::Json(doc, bad), yapi::ApiError);
}

TEST(Patch, StrategicMergesByKey) {
    auto status = json::FromString(R"({"status":{"conditions":[{"type":"Ready","status":"False"},{"type":"Scheduled","status":"True"}]}})");
    auto p = json::FromString(R"({"status":{"$setElementOrder/conditions":[{"type":"Ready"},{"type":"Scheduled"}],"conditions":[{"type":"Ready","status":"True"}]}})");
    auto out = yapi::patch::Strategic(status, p);
    EXPECT_EQ(out["status"]["conditions"].GetSize(), 2u);
    EXPECT_EQ(out["status"]["conditions"][0]["status"].As<std::string>(), "True");
    EXPECT_FALSE(out["status"].HasMember("$setElementOrder/conditions"));
}

TEST(Patch, StrategicDirectives) {
    auto obj = json::FromString(R"({"metadata":{"finalizers":["a","b"]},"spec":{"containers":[{"name":"x","image":"1"},{"name":"y","image":"2"}]}})");
    auto p = json::FromString(R"({"metadata":{"finalizers":["c"],"$deleteFromPrimitiveList/finalizers":["a"]},"spec":{"containers":[{"name":"y","$patch":"delete"}]}})");
    auto out = yapi::patch::Strategic(obj, p);
    EXPECT_EQ(out["metadata"]["finalizers"], json::FromString(R"(["b","c"])"));
    EXPECT_EQ(out["spec"]["containers"].GetSize(), 1u);
    EXPECT_EQ(out["spec"]["containers"][0]["name"].As<std::string>(), "x");
}

TEST(Patch, StrategicReplacesUntaggedLists) {
    auto target = json::FromString(R"({"status":{"containerStatuses":[{"name":"c","state":{"waiting":{"reason":"ContainerCreating"}}}]}})");
    auto patch = R"({"status":{"containerStatuses":[{"name":"c","state":{"running":{"startedAt":"t"}}}]}})";
    auto out = yapi::patch::Apply("application/strategic-merge-patch+json", target, patch);
    EXPECT_EQ(json::ToString(out["status"]["containerStatuses"][0]["state"]), R"({"running":{"startedAt":"t"}})");
}

TEST(Patch, StrategicNullRemovesNestedField) {
    auto target = json::FromString(R"({"spec":{"containers":[{"name":"c","resources":{"limits":{"cpu":"1"}}}]}})");
    auto patch = R"({"spec":{"containers":[{"name":"c","resources":{"limits":null}}]}})";
    auto out = yapi::patch::Apply("application/strategic-merge-patch+json", target, patch);
    EXPECT_EQ(json::ToString(out["spec"]["containers"][0]), R"({"name":"c","resources":{}})");
}
