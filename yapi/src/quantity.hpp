#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace yapi::quantity {

// A resource quantity ("100m", "1.5Gi", "2e3") in base units.
std::optional<long double> Parse(std::string_view text);

// A quantity in base units written the way upstream writes it back: binary
// suffixes for memory-like amounts that divide evenly, milli for fractions.
std::string Format(long double value, bool binary);

}  // namespace yapi::quantity
