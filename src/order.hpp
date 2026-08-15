// order.hpp
//
// Basic value types for the order book: identifiers, sides, order types and
// the Order record itself.
//
// Money is never stored as a floating point number. Every price is an integer
// number of ticks (one tick = one cent by default), because two prices that
// look identical on screen can compare unequal as doubles, and an order book
// compares prices constantly.

#pragma once

#include <cstdint>
#include <string>

namespace obe {

using OrderId  = std::uint64_t;  // unique, assigned by the book on submit
using Price    = std::int64_t;   // price in ticks; 100.25 with 100 ticks/unit -> 10025
using Quantity = std::int64_t;   // number of shares/contracts

// Number of ticks in one price unit (one dollar). 100 means prices are stored
// in cents. Change this in one place if the instrument quotes in finer steps.
inline constexpr Price kTicksPerUnit = 100;

// Price value used for market orders, which have no limit price at all.
inline constexpr Price kNoPrice = 0;

enum class Side { Buy, Sell };

// The order types the engine understands.
//
//   Limit             rest in the book until filled or cancelled
//   Market            take whatever liquidity exists, never rest
//   ImmediateOrCancel take what is available right now, drop the rest
//   FillOrKill        fill the whole quantity immediately or do nothing at all
//   Iceberg           a limit order that shows only a slice of its size
enum class OrderType {
    Limit,
    Market,
    ImmediateOrCancel,
    FillOrKill,
    Iceberg,
};

// One order as the book stores it.
//
// For an iceberg, `remaining` is the full unfilled size (shown plus hidden)
// and `visible` is the slice currently advertised at its price level. Every
// other order type has visible == remaining and display_size == 0.
struct Order {
    OrderId   id           = 0;
    Side      side         = Side::Buy;
    OrderType type         = OrderType::Limit;
    Price     price        = kNoPrice;
    Quantity  remaining    = 0;
    Quantity  visible      = 0;
    Quantity  display_size = 0;  // iceberg slice size; 0 for everything else
};

// One execution. `taker` is the order that arrived and crossed the spread,
// `maker` is the resting order it traded against. The trade always happens at
// the maker's price, which is the standard price-improvement rule.
struct Trade {
    OrderId  taker_id = 0;
    OrderId  maker_id = 0;
    Price    price    = kNoPrice;
    Quantity quantity = 0;
};

// Human readable names, used by the command line tool and the tests.
std::string to_string(Side side);
std::string to_string(OrderType type);

// Convert between the integer tick representation and a decimal string.
// format_price(10025) -> "100.25"; parse_price("100.25") -> 10025.
std::string format_price(Price price);
Price       parse_price(const std::string& text);

}  // namespace obe
