// order_book.cpp - matching, resting and cancelling. See order_book.hpp for
// the data structure choices.

#include "order_book.hpp"

#include <algorithm>
#include <iterator>
#include <stdexcept>

namespace obe {

// Walk the opposite side from the best price outwards, filling the incoming
// order until it is exhausted or the next price no longer crosses.
//
// Book state through one call, buying 400 into asks 100.30 x 300 (order 7)
// and 100.35 x 500 (order 9), incoming limit price 100.35:
//
//   start   incoming.remaining = 400, asks = {100.30: [7 x300], 100.35: [9 x500]}
//   fill 1  300 @ 100.30  -> order 7 gone, level 100.30 removed
//   fill 2  100 @ 100.35  -> order 9 left with 400
//   end     incoming.remaining = 0, asks = {100.35: [9 x400]}
template <typename Ladder>
void OrderBook::match(Ladder&                           ladder,
                      Order&                            incoming,
                      const std::function<bool(Price)>& crosses,
                      std::vector<Trade>&               trades) {
    while (incoming.remaining > 0 && !ladder.empty()) {
        auto        level_it    = ladder.begin();  // best price on this side
        const Price level_price = level_it->first;
        if (!crosses(level_price)) {
            break;
        }

        PriceLevel& level = level_it->second;
        while (incoming.remaining > 0 && !level.orders.empty()) {
            Order&         maker = level.orders.front();  // oldest order wins
            const Quantity fill  = std::min(incoming.remaining, maker.visible);

            incoming.remaining     -= fill;
            maker.visible          -= fill;
            maker.remaining        -= fill;
            level.visible_quantity -= fill;
            level.total_quantity   -= fill;

            // The trade prints at the resting order's price, so the taker gets
            // any price improvement between its limit and the book.
            trades.push_back(Trade{incoming.id, maker.id, level_price, fill});

            if (maker.remaining == 0) {
                locators_.erase(maker.id);
                level.orders.pop_front();
            } else if (maker.visible == 0) {
                // Iceberg: the shown slice is used up, so a fresh slice comes
                // out of hiding and the order loses its place in the queue.
                // splice() moves the node itself, so the iterator kept in
                // locators_ stays valid and nothing is copied.
                const Quantity slice = std::min(maker.display_size, maker.remaining);
                maker.visible           = slice;
                level.visible_quantity += slice;

                level.orders.splice(level.orders.end(), level.orders, level.orders.begin());
                locators_[maker.id].position = std::prev(level.orders.end());
            }
        }

        if (level.orders.empty()) {
            ladder.erase(level_it);
        }
    }
}

void OrderBook::rest(const Order& order) {
    Order stored = order;
    // An iceberg shows one slice; everything else shows its whole size.
    stored.visible = stored.display_size > 0
                         ? std::min(stored.display_size, stored.remaining)
                         : stored.remaining;

    auto place = [&](auto& ladder) {
        PriceLevel& level = ladder[stored.price];
        level.orders.push_back(stored);
        level.visible_quantity += stored.visible;
        level.total_quantity   += stored.remaining;
        locators_[stored.id] =
            Locator{stored.side, stored.price, std::prev(level.orders.end())};
    };

    if (stored.side == Side::Buy) {
        place(bids_);
    } else {
        place(asks_);
    }
}

SubmitResult OrderBook::submit(Side      side,
                               OrderType type,
                               Price     price,
                               Quantity  quantity,
                               Quantity  display_size) {
    if (quantity <= 0) {
        throw std::invalid_argument("order quantity must be positive");
    }
    if (type == OrderType::Iceberg && display_size <= 0) {
        throw std::invalid_argument("iceberg order needs a positive display size");
    }

    const bool is_market = type == OrderType::Market;

    Order incoming;
    incoming.id           = next_order_id_++;
    incoming.side         = side;
    incoming.type         = type;
    incoming.price        = is_market ? kNoPrice : price;
    incoming.remaining    = quantity;
    incoming.visible      = quantity;
    incoming.display_size = type == OrderType::Iceberg ? display_size : 0;

    SubmitResult result;
    result.id = incoming.id;

    // Fill-or-kill is checked before anything trades: if the whole size cannot
    // be filled right now, the order is rejected and the book is untouched.
    // Hidden iceberg size counts here, because it is real liquidity.
    if (type == OrderType::FillOrKill &&
        available_quantity(side, incoming.price, false) < quantity) {
        result.killed = true;
        return result;
    }

    if (side == Side::Buy) {
        match(
            asks_, incoming,
            [&](Price level_price) { return is_market || level_price <= incoming.price; },
            result.trades);
    } else {
        match(
            bids_, incoming,
            [&](Price level_price) { return is_market || level_price >= incoming.price; },
            result.trades);
    }

    // Only limit and iceberg orders wait in the book. Market, immediate-or-
    // cancel and fill-or-kill remainders are dropped.
    if (incoming.remaining > 0 &&
        (type == OrderType::Limit || type == OrderType::Iceberg)) {
        rest(incoming);
        result.resting = true;
    }

    return result;
}

bool OrderBook::cancel(OrderId id) {
    const auto found = locators_.find(id);
    if (found == locators_.end()) {
        return false;  // unknown id, or the order is already fully filled
    }

    const Locator locator = found->second;
    locators_.erase(found);

    auto remove_from = [&](auto& ladder) {
        const auto level_it = ladder.find(locator.price);
        if (level_it == ladder.end()) {
            return;
        }
        PriceLevel& level = level_it->second;
        level.visible_quantity -= locator.position->visible;
        level.total_quantity   -= locator.position->remaining;
        level.orders.erase(locator.position);
        if (level.orders.empty()) {
            ladder.erase(level_it);
        }
    };

    if (locator.side == Side::Buy) {
        remove_from(bids_);
    } else {
        remove_from(asks_);
    }
    return true;
}

std::optional<Price> OrderBook::best_bid() const {
    if (bids_.empty()) {
        return std::nullopt;
    }
    return bids_.begin()->first;
}

std::optional<Price> OrderBook::best_ask() const {
    if (asks_.empty()) {
        return std::nullopt;
    }
    return asks_.begin()->first;
}

std::optional<Price> OrderBook::spread() const {
    const auto bid = best_bid();
    const auto ask = best_ask();
    if (!bid || !ask) {
        return std::nullopt;
    }
    return *ask - *bid;
}

Quantity OrderBook::available_quantity(Side side, Price limit, bool is_market) const {
    Quantity total = 0;

    auto accumulate = [&](const auto& ladder, auto crosses) {
        for (const auto& [level_price, level] : ladder) {
            if (!is_market && !crosses(level_price)) {
                break;  // ladders are price ordered, so nothing further crosses
            }
            total += level.total_quantity;
        }
    };

    if (side == Side::Buy) {
        accumulate(asks_, [&](Price level_price) { return level_price <= limit; });
    } else {
        accumulate(bids_, [&](Price level_price) { return level_price >= limit; });
    }
    return total;
}

std::vector<LevelView> OrderBook::snapshot(Side side, std::size_t depth) const {
    std::vector<LevelView> rows;
    rows.reserve(depth);

    auto collect = [&](const auto& ladder) {
        for (const auto& [level_price, level] : ladder) {
            if (rows.size() >= depth) {
                break;
            }
            rows.push_back(LevelView{level_price, level.visible_quantity,
                                     level.total_quantity, level.orders.size()});
        }
    };

    if (side == Side::Buy) {
        collect(bids_);
    } else {
        collect(asks_);
    }
    return rows;
}

}  // namespace obe
