// order_book.hpp
//
// A single-instrument limit order book with price-time priority.
//
// What it can do
// --------------
//   submit    add a limit, market, immediate-or-cancel, fill-or-kill, iceberg
//             or post-only order and match it immediately
//   cancel    pull a resting order out of the queue
//   modify    change the price or size of a resting order, applying the
//             standard queue priority rules
//   order     look up any order ever submitted, resting or finished
//   snapshot  read the top price levels of either side
//   statistics last trade price, traded volume, high and low
//   validate  check every internal invariant, used by the random tester
//
// Data structures and why they were chosen
// ----------------------------------------
// Price priority: two ordered maps, one per side (`bids_` sorted high to low,
//   `asks_` low to high). The first element is always the best price, so the
//   book behaves like a priority queue keyed on price, while still allowing
//   O(log n) lookup and erase of any price level by key. A plain binary heap
//   cannot delete an arbitrary element, which cancels require.
//
// Time priority: each price level holds a std::list of orders, which is a
//   doubly linked list. New orders join the back, matching consumes the front,
//   and an iceberg that refills is moved to the back with list::splice in O(1)
//   without invalidating any iterator or copying the order.
//
// Cancels: `locators_` maps an order id to (side, price, iterator into that
//   level's list), so cancelling is O(log n) for the map lookup of the level
//   and O(1) for the list erase. This is the reason the queue is a linked
//   list rather than a vector: erasing from the middle of a vector would move
//   every element behind it and invalidate the stored positions.
//
// Memory note: `finished_` keeps a record of every order that has left the
//   book, so `order(id)` can still answer questions about it. That grows with
//   the number of orders processed, which is fine for a simulator and would
//   be replaced by a rolling log in a real system.

#pragma once

#include <cstddef>
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "order.hpp"

namespace obe {

// One resting queue at a single price.
//
// `visible_quantity` is what the market sees; `total_quantity` includes the
// hidden part of any iceberg sitting here. The two are equal unless an
// iceberg is present.
struct PriceLevel {
    std::list<Order> orders;
    Quantity         visible_quantity = 0;
    Quantity         total_quantity   = 0;
};

// What one submit() or modify() call did.
//
//   id       the id of the order the call acted on
//   trades   executions produced, in the order they happened
//   status   final state of that order after the call
//   resting  true if an unfilled remainder is now sitting in the book
struct SubmitResult {
    OrderId            id      = 0;
    std::vector<Trade> trades;
    OrderStatus        status  = OrderStatus::New;
    bool               resting = false;

    Quantity filled_quantity() const;
};

// What one modify() call did. `found` is false when the id is not resting,
// which also covers an order that was already filled or cancelled.
struct ModifyResult {
    bool         found = false;
    bool         kept_queue_position = false;  // true for a pure size reduction
    SubmitResult outcome;
};

// A read-only snapshot row, used for printing the book.
struct LevelView {
    Price       price            = kNoPrice;
    Quantity    visible_quantity = 0;
    Quantity    total_quantity   = 0;
    std::size_t order_count      = 0;
};

// Running market data derived from the trades the book has printed.
struct MarketStatistics {
    Quantity             traded_volume = 0;
    std::size_t          trade_count   = 0;
    std::optional<Price> last_trade_price;
    std::optional<Price> high_trade_price;
    std::optional<Price> low_trade_price;
};

class OrderBook {
public:
    // Called once per execution, before submit() returns. Optional; it exists
    // so a simulator or a market data feed can watch trades as they happen.
    using TradeListener = std::function<void(const Trade&)>;

    // Submit a new order and match it against the opposite side immediately.
    //
    // price        ignored for OrderType::Market
    // quantity     must be positive
    // display_size iceberg slice; required (> 0) for OrderType::Iceberg and
    //              ignored for every other type
    //
    // Throws std::invalid_argument on a non-positive quantity or an iceberg
    // without a usable display size.
    SubmitResult submit(Side      side,
                        OrderType type,
                        Price     price,
                        Quantity  quantity,
                        Quantity  display_size = 0);

    // Remove a resting order. Returns false if the id is unknown, which also
    // covers an order that has already been fully filled.
    bool cancel(OrderId id);

    // Change a resting order's price or remaining size.
    //
    // new_quantity is the new *remaining* size, not a delta.
    //
    // Queue priority follows the rule every venue uses:
    //   same price and smaller size  -> keeps its place in the queue
    //   anything else                -> goes to the back of the new price
    //                                   level, and can trade on the way there
    //
    // Returns found = false for an unknown or no longer resting id. Throws
    // std::invalid_argument on a non-positive new_quantity.
    ModifyResult modify(OrderId id, Price new_price, Quantity new_quantity);

    // Look up any order the book has ever seen, resting or finished.
    std::optional<OrderState> order(OrderId id) const;

    std::optional<Price> best_bid() const;
    std::optional<Price> best_ask() const;

    // Best ask minus best bid, empty if either side is empty.
    std::optional<Price> spread() const;

    // Midpoint of the two best prices, rounded down to the nearest tick.
    // Empty if either side is empty.
    std::optional<Price> mid_price() const;

    // Total quantity that could trade against a taker of `side` at `limit`,
    // hidden iceberg size included. With is_market = true the limit is
    // ignored and every level on the opposite side counts.
    Quantity available_quantity(Side side, Price limit, bool is_market) const;

    // Resting quantity at one exact price on one side, hidden size included.
    // Zero if nothing is resting there.
    Quantity quantity_at_price(Side side, Price price) const;

    // All resting quantity on one side, hidden size included.
    Quantity resting_quantity(Side side) const;

    // Top `depth` levels of one side, best price first.
    std::vector<LevelView> snapshot(Side side, std::size_t depth) const;

    const MarketStatistics& statistics() const { return statistics_; }

    std::size_t resting_order_count() const { return locators_.size(); }

    void set_trade_listener(TradeListener listener) { trade_listener_ = std::move(listener); }

    // Check every internal invariant and return a list of problems found.
    // An empty list means the book is consistent. Checked here:
    //   1. the book is not crossed (best bid < best ask)
    //   2. each level's cached quantities equal the sum of its orders
    //   3. no empty level and no order with zero or negative size
    //   4. every resting order has a locator pointing back at it
    //   5. the locator count matches the number of resting orders
    // This is what the random order tester calls after every instruction.
    std::vector<std::string> validate() const;

private:
    // Bids are sorted high to low so begin() is the best bid; asks low to high
    // so begin() is the best ask.
    using BidLadder = std::map<Price, PriceLevel, std::greater<Price>>;
    using AskLadder = std::map<Price, PriceLevel, std::less<Price>>;

    // Where a resting order lives, so cancel() and modify() reach it directly.
    struct Locator {
        Side                       side;
        Price                      price;
        std::list<Order>::iterator position;
    };

    // Shared path for a new order and for the re-entry half of a modify: run
    // the pre-trade checks, match, then rest whatever is left.
    SubmitResult execute(Order incoming);

    // Match `incoming` against one side of the book. `crosses` decides whether
    // a given resting price is tradable for this order. Fills are appended to
    // `trades` and `incoming.remaining` is reduced in place.
    template <typename Ladder>
    void match(Ladder&                           ladder,
               Order&                            incoming,
               const std::function<bool(Price)>& crosses,
               std::vector<Trade>&               trades);

    // Place the unfilled remainder of `order` at the back of its price level.
    void rest(const Order& order);

    // Take a resting order out of the book and return it. The caller must
    // know the id is resting.
    Order detach(OrderId id);

    // Record an order that has left the book, so order(id) can still find it.
    void remember_finished(const Order& order, OrderStatus status);

    void record_trade(const Trade& trade);

    BidLadder bids_;
    AskLadder asks_;
    std::unordered_map<OrderId, Locator>    locators_;
    std::unordered_map<OrderId, OrderState> finished_;
    MarketStatistics                        statistics_;
    TradeListener                           trade_listener_;
    OrderId  next_order_id_ = 1;
    Sequence next_sequence_ = 1;
};

// Build the public view of an order from the record the book stores.
OrderState state_of(const Order& order, bool resting);

}  // namespace obe
