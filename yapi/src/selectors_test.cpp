#include <selectors.hpp>

#include <userver/formats/json/serialize.hpp>
#include <userver/utest/utest.hpp>

#include <status.hpp>

namespace json = userver::formats::json;

TEST(LabelSelector, Terms) {
    auto labels = json::FromString(R"({"app":"web","tier":"front"})");
    EXPECT_TRUE(yapi::LabelSelector::Parse("app=web").Matches(labels));
    EXPECT_TRUE(yapi::LabelSelector::Parse("app==web,tier").Matches(labels));
    EXPECT_FALSE(yapi::LabelSelector::Parse("app!=web").Matches(labels));
    EXPECT_TRUE(yapi::LabelSelector::Parse("tier in (front, back)").Matches(labels));
    EXPECT_FALSE(yapi::LabelSelector::Parse("tier notin (front)").Matches(labels));
    EXPECT_TRUE(yapi::LabelSelector::Parse("!missing").Matches(labels));
    EXPECT_FALSE(yapi::LabelSelector::Parse("missing").Matches(labels));
    EXPECT_TRUE(yapi::LabelSelector::Parse("").Matches(json::Value()));
    EXPECT_TRUE(yapi::LabelSelector::Parse("missing!=x").Matches(labels));
}

TEST(FieldSelector, Paths) {
    auto pod = json::FromString(R"({"metadata":{"name":"p","namespace":"n"},"spec":{"nodeName":"a"},"status":{"phase":"Running"}})");
    EXPECT_TRUE(yapi::FieldSelector::Parse("spec.nodeName=a").Matches(pod));
    EXPECT_FALSE(yapi::FieldSelector::Parse("spec.nodeName=b").Matches(pod));
    EXPECT_TRUE(yapi::FieldSelector::Parse("status.phase!=Failed,metadata.name==p").Matches(pod));
    EXPECT_TRUE(yapi::FieldSelector::Parse("spec.nodeName!=").Matches(pod));
    EXPECT_EQ(yapi::FieldSelector::Parse("metadata.namespace=n").Pinned("metadata.namespace"), "n");
    EXPECT_THROW(yapi::FieldSelector::Parse("nonsense"), yapi::ApiError);
}
