// test_order_book.cpp
//
// Checks for the matching rules. No test framework: each case is a function
// with a name, run by main(), and a failure prints the file and line then
// exits non-zero.

#include <cstdlib>
#include <iostream>
#include <string>

#include "order.hpp"
#include "order_book.hpp"

namespace {

int failures = 0;

void check(bool condition, const std::string& what, int line) {
    if (!condition) {
        ++failures;
        std::cout << "FAIL (line " << line << "): " << what << "\n";
    }
}

#define CHECK(condition) check((condition), #condition, __LINE__)

using obe::OrderBook;
using obe::OrderType;
using obe::Side;

// 100.25 written the way the engine stores it.
obe::Price px(const std::string& text) { return obe::parse_price(text); }

// A resting limit order that nothing crosses stays in the book and sets the
// best price on its side.
void test_limit_order_rests() {
    OrderBook book;
    const auto result = book.submit(Side::Buy, OrderType::Limit, px("100.25"), 500);

    CHECK(result.trades.empty());
    CHECK(result.resting);
    CHECK(book.best_bid().value() == px("100.25"));
    CHECK(!book.best_ask().has_value());
    CHECK(book.resting_order_count() == 1);
}

// Two orders at the same price fill in arrival order, oldest first.
void test_time_priority_within_a_price() {
    OrderBook book;
    const auto first  = book.submit(Side::Sell, OrderType::Limit, px("100.30"), 100);
    const auto second = book.submit(Side::Sell, OrderType::Limit, px("100.30"), 100);

    const auto taker = book.submit(Side::Buy, OrderType::Market, obe::kNoPrice, 150);

    CHECK(taker.trades.size() == 2);
    CHECK(taker.trades[0].maker_id == first.id);
    CHECK(taker.trades[0].quantity == 100);
    CHECK(taker.trades[1].maker_id == second.id);
    CHECK(taker.trades[1].quantity == 50);
}

// A taker sweeps the cheapest ask first and trades at each maker's price, not
// at its own limit.
void test_price_priority_and_maker_price() {
    OrderBook book;
    book.submit(Side::Sell, OrderType::Limit, px("100.35"), 300);
    book.submit(Side::Sell, OrderType::Limit, px("100.30"), 200);

    const auto taker = book.submit(Side::Buy, OrderType::Limit, px("100.40"), 400);

    CHECK(taker.trades.size() == 2);
    CHECK(taker.trades[0].price == px("100.30"));
    CHECK(taker.trades[0].quantity == 200);
    CHECK(taker.trades[1].price == px("100.35"));
    CHECK(taker.trades[1].quantity == 200);
    CHECK(!taker.resting);
    CHECK(book.best_ask().value() == px("100.35"));
}

// Immediate-or-cancel takes what is there and throws the rest away.
void test_immediate_or_cancel_drops_the_remainder() {
    OrderBook book;
    book.submit(Side::Sell, OrderType::Limit, px("100.30"), 100);

    const auto taker =
        book.submit(Side::Buy, OrderType::ImmediateOrCancel, px("100.30"), 400);

    CHECK(taker.trades.size() == 1);
    CHECK(taker.trades[0].quantity == 100);
    CHECK(!taker.resting);
    CHECK(book.resting_order_count() == 0);
}

// Fill-or-kill with too little liquidity trades nothing and leaves the book
// exactly as it was.
void test_fill_or_kill_rejects_a_partial() {
    OrderBook book;
    book.submit(Side::Sell, OrderType::Limit, px("100.30"), 100);

    const auto taker = book.submit(Side::Buy, OrderType::FillOrKill, px("100.30"), 400);

    CHECK(taker.killed);
    CHECK(taker.trades.empty());
    CHECK(book.best_ask().value() == px("100.30"));
    CHECK(book.resting_order_count() == 1);
}

// Enough liquidity across two levels, so the same order fills completely.
void test_fill_or_kill_fills_completely() {
    OrderBook book;
    book.submit(Side::Sell, OrderType::Limit, px("100.30"), 100);
    book.submit(Side::Sell, OrderType::Limit, px("100.35"), 300);

    const auto taker = book.submit(Side::Buy, OrderType::FillOrKill, px("100.35"), 400);

    CHECK(!taker.killed);
    CHECK(taker.trades.size() == 2);
    CHECK(book.resting_order_count() == 0);
}

// Liquidity priced above the limit does not count towards a fill-or-kill.
void test_fill_or_kill_ignores_liquidity_beyond_the_limit() {
    OrderBook book;
    book.submit(Side::Sell, OrderType::Limit, px("100.30"), 100);
    book.submit(Side::Sell, OrderType::Limit, px("100.50"), 900);

    const auto taker = book.submit(Side::Buy, OrderType::FillOrKill, px("100.35"), 400);

    CHECK(taker.killed);
    CHECK(taker.trades.empty());
}

// An iceberg shows only its slice, but the hidden part still trades.
void test_iceberg_shows_a_slice_and_fills_in_full() {
    OrderBook book;
    book.submit(Side::Sell, OrderType::Iceberg, px("100.30"), 1000, 200);

    const auto shown = book.snapshot(Side::Sell, 1);
    CHECK(shown.size() == 1);
    CHECK(shown[0].visible_quantity == 200);
    CHECK(shown[0].total_quantity == 1000);

    const auto taker = book.submit(Side::Buy, OrderType::Market, obe::kNoPrice, 500);

    obe::Quantity filled = 0;
    for (const auto& trade : taker.trades) {
        filled += trade.quantity;
    }
    CHECK(filled == 500);

    // 500 taken from a 200 slice is two full slices plus 100 of a third, so
    // 100 of the current slice is still showing. A slice is only refreshed
    // once it is completely used up.
    const auto left = book.snapshot(Side::Sell, 1);
    CHECK(left[0].total_quantity == 500);
    CHECK(left[0].visible_quantity == 100);
}

// When an iceberg refills, it goes to the back of the queue, so an ordinary
// order that arrived later still trades before the refreshed slice.
void test_iceberg_loses_time_priority_on_refill() {
    OrderBook book;
    const auto iceberg = book.submit(Side::Sell, OrderType::Iceberg, px("100.30"), 1000, 100);
    const auto plain   = book.submit(Side::Sell, OrderType::Limit, px("100.30"), 100);

    const auto taker = book.submit(Side::Buy, OrderType::Market, obe::kNoPrice, 250);

    CHECK(taker.trades.size() == 3);
    CHECK(taker.trades[0].maker_id == iceberg.id);   // first shown slice
    CHECK(taker.trades[1].maker_id == plain.id);     // queued behind that slice
    CHECK(taker.trades[2].maker_id == iceberg.id);   // refreshed slice, now last
    CHECK(taker.trades[2].quantity == 50);
}

// A cancelled order stops trading, and its price level disappears when empty.
void test_cancel_removes_the_order_and_the_level() {
    OrderBook book;
    const auto resting = book.submit(Side::Buy, OrderType::Limit, px("100.25"), 500);

    CHECK(book.cancel(resting.id));
    CHECK(!book.best_bid().has_value());
    CHECK(book.resting_order_count() == 0);
    CHECK(!book.cancel(resting.id));  // cancelling twice is rejected, not a crash
}

// Cancelling one order at a shared price leaves the other one trading.
void test_cancel_keeps_the_rest_of_the_queue() {
    OrderBook book;
    const auto first  = book.submit(Side::Buy, OrderType::Limit, px("100.25"), 500);
    const auto second = book.submit(Side::Buy, OrderType::Limit, px("100.25"), 300);

    CHECK(book.cancel(first.id));

    const auto shown = book.snapshot(Side::Buy, 1);
    CHECK(shown[0].visible_quantity == 300);

    const auto taker = book.submit(Side::Sell, OrderType::Market, obe::kNoPrice, 300);
    CHECK(taker.trades.size() == 1);
    CHECK(taker.trades[0].maker_id == second.id);
}

// A market order with an empty book does nothing and does not rest.
void test_market_order_on_an_empty_book() {
    OrderBook book;
    const auto taker = book.submit(Side::Buy, OrderType::Market, obe::kNoPrice, 100);

    CHECK(taker.trades.empty());
    CHECK(!taker.resting);
    CHECK(book.resting_order_count() == 0);
}

// Prices survive the round trip between text and integer ticks.
void test_price_formatting() {
    CHECK(px("100.25") == 10025);
    CHECK(px("100.05") == 10005);
    CHECK(px("100") == 10000);
    CHECK(obe::format_price(10005) == "100.05");
    CHECK(obe::format_price(10025) == "100.25");
}

// Bad input is rejected with an exception instead of corrupting the book.
void test_invalid_orders_throw() {
    OrderBook book;
    bool threw = false;
    try {
        book.submit(Side::Buy, OrderType::Limit, px("100.25"), 0);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);

    threw = false;
    try {
        book.submit(Side::Buy, OrderType::Iceberg, px("100.25"), 100, 0);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
    CHECK(book.resting_order_count() == 0);
}

}  // namespace

int main() {
    test_limit_order_rests();
    test_time_priority_within_a_price();
    test_price_priority_and_maker_price();
    test_immediate_or_cancel_drops_the_remainder();
    test_fill_or_kill_rejects_a_partial();
    test_fill_or_kill_fills_completely();
    test_fill_or_kill_ignores_liquidity_beyond_the_limit();
    test_iceberg_shows_a_slice_and_fills_in_full();
    test_iceberg_loses_time_priority_on_refill();
    test_cancel_removes_the_order_and_the_level();
    test_cancel_keeps_the_rest_of_the_queue();
    test_market_order_on_an_empty_book();
    test_price_formatting();
    test_invalid_orders_throw();

    if (failures == 0) {
        std::cout << "all tests passed\n";
        return EXIT_SUCCESS;
    }
    std::cout << failures << " check(s) failed\n";
    return EXIT_FAILURE;
}
