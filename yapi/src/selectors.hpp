#pragma once

#include <string>
#include <vector>

#include <userver/formats/json/value.hpp>

namespace yapi {

// A label selector in its query-string form: "a=b,c!=d,e in (f,g),!h".
class LabelSelector {
public:
    static LabelSelector Parse(const std::string& text);  // throws ApiError on bad syntax

    bool Matches(const userver::formats::json::Value& labels) const;
    bool Empty() const { return terms_.empty(); }

private:
    enum class Op { kEq, kNotEq, kIn, kNotIn, kExists, kNotExists };
    struct Term {
        std::string key;
        Op op;
        std::vector<std::string> values;
    };
    std::vector<Term> terms_;
};

// A field selector: "metadata.name=x,spec.nodeName!=y". Paths are read from
// the object as JSON, so any field works, not only the ones upstream indexes.
class FieldSelector {
public:
    static FieldSelector Parse(const std::string& text);

    bool Matches(const userver::formats::json::Value& object) const;
    bool Empty() const { return terms_.empty(); }

    // The value the selector pins a field to, when it does: lets a list of a
    // namespace or a name skip the scan.
    std::string Pinned(const std::string& path) const;

private:
    struct Term {
        std::string path;
        bool equal;
        std::string value;
    };
    std::vector<Term> terms_;
};

}  // namespace yapi
