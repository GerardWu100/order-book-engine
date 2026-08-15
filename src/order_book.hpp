// order_book.hpp
//
// A single-instrument limit order book with price-time priority.
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

#pragma once

#include <cstddef>
#include <functional>
#include <list>
#include <map>
#include <optional>
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

// What one submit() call did.
//
//   id       the id assigned to the incoming order
//   trades   executions produced, in the order they happened
//   resting  true if an unfilled remainder was placed in the book
//   killed   true if a fill-or-kill order was rejected untouched
struct SubmitResult {
    OrderId            id      = 0;
    std::vector<Trade> trades;
    bool               resting = false;
    bool               killed  = false;
};

// A read-only snapshot row, used for printing the book.
struct LevelView {
    Price       price            = kNoPrice;
    Quantity    visible_quantity = 0;
    Quantity    total_quantity   = 0;
    std::size_t order_count      = 0;
};

class OrderBook {
public:
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

    std::optional<Price> best_bid() const;
    std::optional<Price> best_ask() const;

    // Best ask minus best bid, empty if either side is empty.
    std::optional<Price> spread() const;

    // Total quantity that could trade against a taker of `side` at `limit`,
    // hidden iceberg size included. Passing kNoPrice with a market order in
    // mind means "every level on the opposite side".
    Quantity available_quantity(Side side, Price limit, bool is_market) const;

    // Top `depth` levels of one side, best price first.
    std::vector<LevelView> snapshot(Side side, std::size_t depth) const;

    std::size_t resting_order_count() const { return locators_.size(); }

private:
    // Bids are sorted high to low so begin() is the best bid; asks low to high
    // so begin() is the best ask.
    using BidLadder = std::map<Price, PriceLevel, std::greater<Price>>;
    using AskLadder = std::map<Price, PriceLevel, std::less<Price>>;

    // Where a resting order lives, so cancel() can reach it directly.
    struct Locator {
        Side                       side;
        Price                      price;
        std::list<Order>::iterator position;
    };

    // Match `incoming` against one side of the book. `crosses` decides whether
    // a given resting price is tradable for this order. Fills are appended to
    // `trades` and `incoming.remaining` is reduced in place.
    template <typename Ladder>
    void match(Ladder&                             ladder,
               Order&                              incoming,
               const std::function<bool(Price)>&   crosses,
               std::vector<Trade>&                 trades);

    // Place the unfilled remainder of `order` at the back of its price level.
    void rest(const Order& order);

    BidLadder bids_;
    AskLadder asks_;
    std::unordered_map<OrderId, Locator> locators_;
    OrderId next_order_id_ = 1;
};

}  // namespace obe
