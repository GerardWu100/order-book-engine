// order.cpp - naming and price formatting helpers declared in order.hpp.

#include "order.hpp"

#include <cstdlib>
#include <stdexcept>

namespace obe {

std::string to_string(Side side) {
    return side == Side::Buy ? "BUY" : "SELL";
}

std::string to_string(OrderType type) {
    switch (type) {
        case OrderType::Limit:             return "LIMIT";
        case OrderType::Market:            return "MARKET";
        case OrderType::ImmediateOrCancel: return "IOC";
        case OrderType::FillOrKill:        return "FOK";
        case OrderType::Iceberg:           return "ICEBERG";
    }
    return "UNKNOWN";
}

// 10025 -> "100.25". The fractional part is padded so 10005 gives "100.05"
// rather than "100.5".
std::string format_price(Price price) {
    const Price units     = price / kTicksPerUnit;
    const Price remainder = price % kTicksPerUnit;

    std::string fraction = std::to_string(remainder);
    const std::size_t width = std::to_string(kTicksPerUnit - 1).size();
    while (fraction.size() < width) {
        fraction.insert(fraction.begin(), '0');
    }
    return std::to_string(units) + "." + fraction;
}

// "100.25" -> 10025. Accepts a whole number too ("100" -> 10000). Extra
// decimals beyond the tick size are truncated, not rounded, so a bad input
// can never invent liquidity at a better price.
Price parse_price(const std::string& text) {
    const std::size_t dot = text.find('.');
    if (dot == std::string::npos) {
        return std::stoll(text) * kTicksPerUnit;
    }

    const std::string units_text    = text.substr(0, dot);
    std::string       fraction_text = text.substr(dot + 1);

    const std::size_t width = std::to_string(kTicksPerUnit - 1).size();
    fraction_text.resize(width, '0');  // pad or truncate to the tick width

    const Price units    = units_text.empty() ? 0 : std::stoll(units_text);
    const Price fraction = std::stoll(fraction_text);
    return units * kTicksPerUnit + fraction;
}

}  // namespace obe
