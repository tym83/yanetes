#include <objects.hpp>

#include <userver/formats/json/serialize.hpp>
#include <userver/utest/utest.hpp>

#include <registry.hpp>
#include <status.hpp>

namespace json = userver::formats::json;

namespace {
const yapi::Resource& Pods() { return *yapi::Registry::Builtin().Find("", "v1", "pods"); }
}  // namespace

TEST(Objects, CreateFillsServerFields) {
    auto in = json::FromString(R"({"metadata":{"generateName":"web-"},"spec":{"containers":[{"name":"c","image":"nginx"}]}})");
    auto out = yapi::objects::PrepareCreate(Pods(), in, "default");
    EXPECT_TRUE(out["metadata"]["name"].As<std::string>().starts_with("web-"));
    EXPECT_EQ(out["metadata"]["namespace"].As<std::string>(), "default");
    EXPECT_FALSE(out["metadata"]["uid"].As<std::string>().empty());
    EXPECT_EQ(out["metadata"]["generation"].As<int>(), 1);
    EXPECT_EQ(out["kind"].As<std::string>(), "Pod");
    EXPECT_EQ(out["status"]["phase"].As<std::string>(), "Pending");
    EXPECT_EQ(out["spec"]["restartPolicy"].As<std::string>(), "Always");
    EXPECT_EQ(out["spec"]["containers"][0]["imagePullPolicy"].As<std::string>(), "Always");
}

TEST(Objects, CreateRejectsBadNames) {
    auto in = json::FromString(R"({"metadata":{"name":"Bad_Name"}})");
    EXPECT_THROW(yapi::objects::PrepareCreate(Pods(), in, "default"), yapi::ApiError);
    auto none = json::FromString(R"({"metadata":{}})");
    EXPECT_THROW(yapi::objects::PrepareCreate(Pods(), none, "default"), yapi::ApiError);
}

TEST(Objects, UpdateSplitsSpecAndStatus) {
    auto stored = yapi::objects::PrepareCreate(
        Pods(), json::FromString(R"({"metadata":{"name":"p"},"spec":{"containers":[{"name":"c","image":"a:1"}]}})"), "d"
    );
    auto main = json::FromString(R"({"metadata":{"name":"p","uid":"x"},"spec":{"containers":[{"name":"c","image":"a:2"}]},"status":{"phase":"Running"}})");
    auto updated = yapi::objects::PrepareUpdate(Pods(), stored, main, false);
    EXPECT_EQ(updated["status"]["phase"].As<std::string>(), "Pending");
    EXPECT_EQ(updated["metadata"]["uid"], stored["metadata"]["uid"]);
    EXPECT_EQ(updated["metadata"]["generation"].As<int>(), 2);

    auto status = json::FromString(R"({"metadata":{"name":"p"},"spec":{},"status":{"phase":"Running"}})");
    auto written = yapi::objects::PrepareUpdate(Pods(), stored, status, true);
    EXPECT_EQ(written["status"]["phase"].As<std::string>(), "Running");
    EXPECT_EQ(written["spec"], stored["spec"]);
    EXPECT_EQ(written["metadata"]["generation"].As<int>(), 1);
}
