// test_order_book.cpp
//
// Checks for the matching rules, the order lifecycle and the random order
// generator. No test framework: each case is a function with a name, run by
// main(), and a failure prints the line number then exits non-zero.

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "order.hpp"
#include "order_book.hpp"
#include "random_flow.hpp"

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
using obe::OrderStatus;
using obe::OrderType;
using obe::Side;

// 100.25 written the way the engine stores it.
obe::Price px(const std::string& text) { return obe::parse_price(text); }

// ---------------------------------------------------------------- resting

// A resting limit order that nothing crosses stays in the book and sets the
// best price on its side.
void test_limit_order_rests() {
    OrderBook book;
    const auto result = book.submit(Side::Buy, OrderType::Limit, px("100.25"), 500);

    CHECK(result.trades.empty());
    CHECK(result.resting);
    CHECK(result.status == OrderStatus::New);
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
    CHECK(taker.status == OrderStatus::Filled);
    CHECK(!taker.resting);
    CHECK(book.best_ask().value() == px("100.35"));
}

// ---------------------------------------------------- immediate order types

// Immediate-or-cancel takes what is there and throws the rest away.
void test_immediate_or_cancel_drops_the_remainder() {
    OrderBook book;
    book.submit(Side::Sell, OrderType::Limit, px("100.30"), 100);

    const auto taker =
        book.submit(Side::Buy, OrderType::ImmediateOrCancel, px("100.30"), 400);

    CHECK(taker.trades.size() == 1);
    CHECK(taker.trades[0].quantity == 100);
    CHECK(taker.status == OrderStatus::PartiallyFilled || taker.status == OrderStatus::Cancelled);
    CHECK(!taker.resting);
    CHECK(book.resting_order_count() == 0);
}

// Fill-or-kill with too little liquidity trades nothing and leaves the book
// exactly as it was.
void test_fill_or_kill_rejects_a_partial() {
    OrderBook book;
    book.submit(Side::Sell, OrderType::Limit, px("100.30"), 100);

    const auto taker = book.submit(Side::Buy, OrderType::FillOrKill, px("100.30"), 400);

    CHECK(taker.status == OrderStatus::Killed);
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

    CHECK(taker.status == OrderStatus::Filled);
    CHECK(taker.trades.size() == 2);
    CHECK(book.resting_order_count() == 0);
}

// Liquidity priced above the limit does not count towards a fill-or-kill.
void test_fill_or_kill_ignores_liquidity_beyond_the_limit() {
    OrderBook book;
    book.submit(Side::Sell, OrderType::Limit, px("100.30"), 100);
    book.submit(Side::Sell, OrderType::Limit, px("100.50"), 900);

    const auto taker = book.submit(Side::Buy, OrderType::FillOrKill, px("100.35"), 400);

    CHECK(taker.status == OrderStatus::Killed);
    CHECK(taker.trades.empty());
}

// A market order with an empty book does nothing and does not rest.
void test_market_order_on_an_empty_book() {
    OrderBook book;
    const auto taker = book.submit(Side::Buy, OrderType::Market, obe::kNoPrice, 100);

    CHECK(taker.trades.empty());
    CHECK(!taker.resting);
    CHECK(taker.status == OrderStatus::Cancelled);
    CHECK(book.resting_order_count() == 0);
}

// ------------------------------------------------------------- post-only

// A post-only order exists to add liquidity. If it would trade on arrival it
// is refused rather than executed.
void test_post_only_rejected_when_it_would_trade() {
    OrderBook book;
    book.submit(Side::Sell, OrderType::Limit, px("100.30"), 500);

    const auto taker = book.submit(Side::Buy, OrderType::PostOnly, px("100.30"), 200);

    CHECK(taker.status == OrderStatus::Rejected);
    CHECK(taker.trades.empty());
    CHECK(!taker.resting);
    CHECK(book.resting_order_count() == 1);
}

// The same order priced away from the touch rests normally.
void test_post_only_rests_when_it_does_not_cross() {
    OrderBook book;
    book.submit(Side::Sell, OrderType::Limit, px("100.30"), 500);

    const auto maker = book.submit(Side::Buy, OrderType::PostOnly, px("100.29"), 200);

    CHECK(maker.status == OrderStatus::New);
    CHECK(maker.resting);
    CHECK(book.best_bid().value() == px("100.29"));
}

// -------------------------------------------------------------- icebergs

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

// ---------------------------------------------------------------- cancels

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

// ------------------------------------------------------------ amendments

// Shrinking an order at the same price is the one change that keeps its place
// in the queue.
void test_modify_smaller_keeps_queue_position() {
    OrderBook book;
    const auto first  = book.submit(Side::Buy, OrderType::Limit, px("100.25"), 500);
    book.submit(Side::Buy, OrderType::Limit, px("100.25"), 300);

    const auto amended = book.modify(first.id, px("100.25"), 200);
    CHECK(amended.found);
    CHECK(amended.kept_queue_position);

    const auto shown = book.snapshot(Side::Buy, 1);
    CHECK(shown[0].total_quantity == 500);  // 200 + 300

    const auto taker = book.submit(Side::Sell, OrderType::Market, obe::kNoPrice, 200);
    CHECK(taker.trades.size() == 1);
    CHECK(taker.trades[0].maker_id == first.id);  // still first in line
}

// Asking for more size sends the order to the back of the queue.
void test_modify_larger_loses_queue_position() {
    OrderBook book;
    const auto first  = book.submit(Side::Buy, OrderType::Limit, px("100.25"), 500);
    const auto second = book.submit(Side::Buy, OrderType::Limit, px("100.25"), 300);

    const auto amended = book.modify(first.id, px("100.25"), 700);
    CHECK(amended.found);
    CHECK(!amended.kept_queue_position);
    CHECK(amended.outcome.resting);

    const auto taker = book.submit(Side::Sell, OrderType::Market, obe::kNoPrice, 300);
    CHECK(taker.trades.size() == 1);
    CHECK(taker.trades[0].maker_id == second.id);  // now at the front
}

// Repricing into the other side trades straight away.
void test_modify_price_can_trade_immediately() {
    OrderBook book;
    const auto bid = book.submit(Side::Buy, OrderType::Limit, px("100.20"), 200);
    book.submit(Side::Sell, OrderType::Limit, px("100.30"), 200);

    const auto amended = book.modify(bid.id, px("100.30"), 200);

    CHECK(amended.found);
    CHECK(!amended.kept_queue_position);
    CHECK(amended.outcome.status == OrderStatus::Filled);
    CHECK(amended.outcome.trades.size() == 1);
    CHECK(book.resting_order_count() == 0);
}

// Amending an order that has already left the book is reported, not applied.
void test_modify_unknown_order() {
    OrderBook book;
    const auto amended = book.modify(999, px("100.25"), 100);
    CHECK(!amended.found);
    CHECK(book.resting_order_count() == 0);
}

// An amendment keeps whatever the order already traded.
void test_modify_keeps_the_filled_quantity() {
    OrderBook book;
    const auto resting = book.submit(Side::Buy, OrderType::Limit, px("100.25"), 500);
    book.submit(Side::Sell, OrderType::Limit, px("100.25"), 200);  // fills 200 of it

    const auto amended = book.modify(resting.id, px("100.24"), 100);
    CHECK(amended.found);

    const auto state = book.order(resting.id);
    CHECK(state.has_value());
    CHECK(state->filled_quantity == 200);
    CHECK(state->remaining_quantity == 100);
    CHECK(state->price == px("100.24"));
}

// -------------------------------------------------------- order lifecycle

// The book can report on an order at every stage of its life.
void test_order_lookup_follows_the_lifecycle() {
    OrderBook book;
    const auto resting = book.submit(Side::Buy, OrderType::Limit, px("100.25"), 500);

    auto state = book.order(resting.id);
    CHECK(state.has_value());
    CHECK(state->status == OrderStatus::New);
    CHECK(state->resting);

    book.submit(Side::Sell, OrderType::Limit, px("100.25"), 200);
    state = book.order(resting.id);
    CHECK(state->status == OrderStatus::PartiallyFilled);
    CHECK(state->filled_quantity == 200);
    CHECK(state->remaining_quantity == 300);

    book.cancel(resting.id);
    state = book.order(resting.id);
    CHECK(state->status == OrderStatus::Cancelled);
    CHECK(!state->resting);
    CHECK(state->filled_quantity == 200);

    CHECK(!book.order(12345).has_value());
}

// ------------------------------------------------------------ market data

void test_statistics_follow_the_trades() {
    OrderBook book;
    CHECK(!book.statistics().vwap().has_value());  // no trades yet

    book.submit(Side::Sell, OrderType::Limit, px("100.30"), 100);
    book.submit(Side::Sell, OrderType::Limit, px("100.40"), 100);
    book.submit(Side::Buy, OrderType::Limit, px("100.45"), 200);

    const auto& stats = book.statistics();
    CHECK(stats.trade_count == 2);
    CHECK(stats.traded_volume == 200);
    CHECK(stats.last_trade_price.value() == px("100.40"));
    CHECK(stats.high_trade_price.value() == px("100.40"));
    CHECK(stats.low_trade_price.value() == px("100.30"));

    // Two equal-sized trades: vwap is the plain average of the two prices.
    CHECK(stats.traded_notional == px("100.30") * 100 + px("100.40") * 100);
    CHECK(stats.vwap().value() == px("100.35"));

    // A third, larger trade must pull the vwap toward its price by volume,
    // not by trade count: (100.30*100 + 100.40*100 + 100.50*300) / 500.
    book.submit(Side::Sell, OrderType::Limit, px("100.50"), 300);
    book.submit(Side::Buy, OrderType::Limit, px("100.50"), 300);
    CHECK(book.statistics().traded_volume == 500);
    CHECK(book.statistics().vwap().value() == px("100.44"));
}

void test_spread_and_mid_price() {
    OrderBook book;
    CHECK(!book.spread().has_value());
    CHECK(!book.mid_price().has_value());

    book.submit(Side::Buy, OrderType::Limit, px("100.20"), 100);
    book.submit(Side::Sell, OrderType::Limit, px("100.30"), 100);

    CHECK(book.spread().value() == px("0.10"));
    CHECK(book.mid_price().value() == px("100.25"));
}

void test_quantity_queries() {
    OrderBook book;
    book.submit(Side::Buy, OrderType::Limit, px("100.20"), 400);
    book.submit(Side::Buy, OrderType::Limit, px("100.25"), 600);
    book.submit(Side::Sell, OrderType::Iceberg, px("100.30"), 1000, 100);

    CHECK(book.quantity_at_price(Side::Buy, px("100.25")) == 600);
    CHECK(book.quantity_at_price(Side::Buy, px("99.00")) == 0);
    CHECK(book.resting_quantity(Side::Buy) == 1000);
    CHECK(book.resting_quantity(Side::Sell) == 1000);  // hidden size counts

    // A buyer at 100.30 can reach the whole iceberg, hidden part included.
    CHECK(book.available_quantity(Side::Buy, px("100.30"), false) == 1000);
    CHECK(book.available_quantity(Side::Buy, px("100.29"), false) == 0);
    CHECK(book.available_quantity(Side::Sell, px("100.20"), false) == 1000);
}

// ---------------------------------------------------------- housekeeping

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

// validate() should find nothing wrong with an ordinary two sided book.
void test_validate_is_quiet_on_a_healthy_book() {
    OrderBook book;
    book.submit(Side::Buy, OrderType::Limit, px("100.20"), 400);
    book.submit(Side::Buy, OrderType::Limit, px("100.25"), 600);
    book.submit(Side::Sell, OrderType::Iceberg, px("100.30"), 1000, 100);
    book.submit(Side::Sell, OrderType::Limit, px("100.35"), 200);
    book.submit(Side::Buy, OrderType::Market, obe::kNoPrice, 250);

    CHECK(book.validate().empty());
}

// ------------------------------------------------------------ random soak

// Run a long stream of random orders, cancels and amendments and check the
// book after every single instruction. This is the test that catches the
// mistakes hand written cases miss: stale cached sizes, a level left behind
// with no orders in it, a crossed book, or quantity appearing from nowhere.
void test_random_flow_keeps_the_book_consistent() {
    constexpr std::size_t kInstructions  = 20000;
    constexpr std::size_t kCompactEvery  = 500;
    constexpr std::size_t kValidateEvery = 25;

    obe::FlowSettings settings;
    settings.seed              = 12345;
    settings.instruction_count = kInstructions;

    OrderBook       book;
    obe::RandomFlow flow(settings);

    std::vector<obe::OrderId> resting_ids;
    std::vector<obe::OrderId> every_id;

    for (std::size_t step = 1; step <= kInstructions; ++step) {
        const obe::Instruction instruction = flow.next(book, resting_ids);

        switch (instruction.kind) {
            case obe::Instruction::Kind::Submit: {
                const auto result =
                    book.submit(instruction.side, instruction.type, instruction.price,
                                instruction.quantity, instruction.display_size);
                every_id.push_back(result.id);
                if (result.resting) {
                    resting_ids.push_back(result.id);
                }
                break;
            }
            case obe::Instruction::Kind::Cancel:
                book.cancel(instruction.target);
                break;
            case obe::Instruction::Kind::Modify: {
                const auto result = book.modify(instruction.target, instruction.new_price,
                                                instruction.new_quantity);
                if (result.found && result.outcome.resting) {
                    resting_ids.push_back(result.outcome.id);
                }
                break;
            }
        }

        if (step % kValidateEvery == 0) {
            const auto problems = book.validate();
            if (!problems.empty()) {
                ++failures;
                std::cout << "FAIL: random flow broke the book at instruction " << step << "\n";
                for (const std::string& problem : problems) {
                    std::cout << "      " << problem << "\n";
                }
                return;
            }
        }

        if (step % kCompactEvery == 0) {
            std::vector<obe::OrderId> live;
            live.reserve(resting_ids.size());
            for (const obe::OrderId id : resting_ids) {
                const auto state = book.order(id);
                if (state && state->resting) {
                    live.push_back(id);
                }
            }
            resting_ids.swap(live);
        }
    }

    // The session has to have done real work, otherwise the checks above prove
    // nothing.
    CHECK(book.statistics().trade_count > 100);
    CHECK(book.resting_order_count() > 0);
    CHECK(book.validate().empty());

    // Every execution adds the same quantity to a buyer and a seller, so the
    // filled quantity summed over every order must be exactly twice the volume
    // the book reports. Quantity cannot be lost or invented anywhere.
    obe::Quantity filled_across_all_orders = 0;
    for (const obe::OrderId id : every_id) {
        if (const auto state = book.order(id)) {
            filled_across_all_orders += state->filled_quantity;
        }
    }
    CHECK(filled_across_all_orders == 2 * book.statistics().traded_volume);
}

// The same seed must produce exactly the same session, otherwise a failure
// cannot be reproduced.
void test_random_flow_is_repeatable() {
    obe::FlowSettings settings;
    settings.seed = 777;

    auto run = [&]() {
        OrderBook       book;
        obe::RandomFlow flow(settings);
        std::vector<obe::OrderId> resting_ids;
        for (std::size_t step = 0; step < 2000; ++step) {
            const auto instruction = flow.next(book, resting_ids);
            if (instruction.kind == obe::Instruction::Kind::Submit) {
                const auto result =
                    book.submit(instruction.side, instruction.type, instruction.price,
                                instruction.quantity, instruction.display_size);
                if (result.resting) {
                    resting_ids.push_back(result.id);
                }
            } else if (instruction.kind == obe::Instruction::Kind::Cancel) {
                book.cancel(instruction.target);
            } else {
                book.modify(instruction.target, instruction.new_price, instruction.new_quantity);
            }
        }
        return book.statistics();
    };

    const auto first  = run();
    const auto second = run();
    CHECK(first.trade_count == second.trade_count);
    CHECK(first.traded_volume == second.traded_volume);
    CHECK(first.last_trade_price == second.last_trade_price);
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
    test_market_order_on_an_empty_book();
    test_post_only_rejected_when_it_would_trade();
    test_post_only_rests_when_it_does_not_cross();
    test_iceberg_shows_a_slice_and_fills_in_full();
    test_iceberg_loses_time_priority_on_refill();
    test_cancel_removes_the_order_and_the_level();
    test_cancel_keeps_the_rest_of_the_queue();
    test_modify_smaller_keeps_queue_position();
    test_modify_larger_loses_queue_position();
    test_modify_price_can_trade_immediately();
    test_modify_unknown_order();
    test_modify_keeps_the_filled_quantity();
    test_order_lookup_follows_the_lifecycle();
    test_statistics_follow_the_trades();
    test_spread_and_mid_price();
    test_quantity_queries();
    test_price_formatting();
    test_invalid_orders_throw();
    test_validate_is_quiet_on_a_healthy_book();
    test_random_flow_keeps_the_book_consistent();
    test_random_flow_is_repeatable();

    if (failures == 0) {
        std::cout << "all tests passed\n";
        return EXIT_SUCCESS;
    }
    std::cout << failures << " check(s) failed\n";
    return EXIT_FAILURE;
}
