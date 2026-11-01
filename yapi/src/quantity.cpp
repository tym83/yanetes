#include <quantity.hpp>

#include <cmath>
#include <cstdlib>
#include <utility>
#include <vector>

namespace yapi::quantity {

std::optional<long double> Parse(std::string_view text) {
    if (text.empty()) return std::nullopt;
    std::string s(text);
    std::size_t end = 0;
    long double number;
    try {
        number = std::stold(s, &end);
    } catch (const std::exception&) {
        return std::nullopt;
    }
    auto suffix = s.substr(end);
    static const std::vector<std::pair<std::string, long double>> kSuffixes = {
        {"", 1},          {"n", 1e-9L},     {"u", 1e-6L},    {"m", 1e-3L},     {"k", 1e3L},
        {"M", 1e6L},      {"G", 1e9L},      {"T", 1e12L},    {"P", 1e15L},     {"E", 1e18L},
        {"Ki", 1024.0L},  {"Mi", 1048576.0L}, {"Gi", 1073741824.0L}, {"Ti", 1099511627776.0L},
        {"Pi", 1125899906842624.0L}, {"Ei", 1152921504606846976.0L},
    };
    for (const auto& [name, factor] : kSuffixes) {
        if (suffix == name) return number * factor;
    }
    return std::nullopt;
}

std::string Format(long double value, bool binary) {
    auto whole = [](long double v) { return std::fabs(v - std::round(v)) < 1e-9L; };
    if (!whole(value)) {
        auto milli = value * 1000;
        if (whole(milli)) return std::to_string(static_cast<long long>(std::llround(milli))) + "m";
        return std::to_string(static_cast<double>(value));
    }
    auto n = static_cast<long long>(std::llround(value));
    if (binary && n != 0) {
        static const std::vector<std::pair<const char*, long long>> kBinary = {
            {"Ei", 1LL << 60}, {"Pi", 1LL << 50}, {"Ti", 1LL << 40}, {"Gi", 1LL << 30}, {"Mi", 1LL << 20}, {"Ki", 1LL << 10},
        };
        for (const auto& [name, factor] : kBinary) {
            if (n % factor == 0) return std::to_string(n / factor) + name;
        }
    }
    return std::to_string(n);
}

}  // namespace yapi::quantity
