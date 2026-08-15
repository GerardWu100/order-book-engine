// order_book.cpp - matching, resting, cancelling, modifying and the
// consistency checks. See order_book.hpp for the data structure choices.

#include "order_book.hpp"

#include <algorithm>
#include <iterator>
#include <stdexcept>

namespace obe {

Quantity SubmitResult::filled_quantity() const {
    Quantity total = 0;
    for (const Trade& trade : trades) {
        total += trade.quantity;
    }
    return total;
}

OrderState state_of(const Order& order, bool resting) {
    OrderState state;
    state.id                 = order.id;
    state.sequence           = order.sequence;
    state.side               = order.side;
    state.type               = order.type;
    state.price              = order.price;
    state.original_quantity  = order.original_quantity;
    state.filled_quantity    = order.original_quantity - order.remaining;
    state.remaining_quantity = order.remaining;
    state.visible_quantity   = order.visible;
    state.display_size       = order.display_size;
    state.resting            = resting;
    state.status = state.filled_quantity > 0 ? OrderStatus::PartiallyFilled : OrderStatus::New;
    return state;
}

void OrderBook::record_trade(const Trade& trade) {
    statistics_.traded_volume += trade.quantity;
    statistics_.trade_count += 1;
    statistics_.last_trade_price = trade.price;

    if (!statistics_.high_trade_price || trade.price > *statistics_.high_trade_price) {
        statistics_.high_trade_price = trade.price;
    }
    if (!statistics_.low_trade_price || trade.price < *statistics_.low_trade_price) {
        statistics_.low_trade_price = trade.price;
    }

    if (trade_listener_) {
        trade_listener_(trade);
    }
}

void OrderBook::remember_finished(const Order& order, OrderStatus status) {
    OrderState state = state_of(order, false);
    state.status     = status;
    finished_[order.id] = state;
}

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
            const Trade trade{incoming.id, maker.id, incoming.side, level_price, fill};
            trades.push_back(trade);
            record_trade(trade);

            if (maker.remaining == 0) {
                remember_finished(maker, OrderStatus::Filled);
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

Order OrderBook::detach(OrderId id) {
    const Locator locator = locators_.at(id);
    Order         taken   = *locator.position;
    locators_.erase(id);

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
    return taken;
}

SubmitResult OrderBook::execute(Order incoming) {
    incoming.sequence = next_sequence_++;

    SubmitResult result;
    result.id = incoming.id;

    const bool is_market = incoming.type == OrderType::Market;

    // A post-only order exists to add liquidity, never to take it, so it is
    // refused outright if the book would let it trade right now.
    if (incoming.type == OrderType::PostOnly) {
        const bool would_trade =
            incoming.side == Side::Buy
                ? (best_ask() && *best_ask() <= incoming.price)
                : (best_bid() && *best_bid() >= incoming.price);
        if (would_trade) {
            result.status = OrderStatus::Rejected;
            remember_finished(incoming, OrderStatus::Rejected);
            return result;
        }
    }

    // Fill-or-kill is checked before anything trades: if the whole size cannot
    // be filled right now, the order is rejected and the book is untouched.
    // Hidden iceberg size counts here, because it is real liquidity.
    if (incoming.type == OrderType::FillOrKill &&
        available_quantity(incoming.side, incoming.price, false) < incoming.remaining) {
        result.status = OrderStatus::Killed;
        remember_finished(incoming, OrderStatus::Killed);
        return result;
    }

    if (incoming.side == Side::Buy) {
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

    // Only limit, iceberg and post-only orders wait in the book. Market,
    // immediate-or-cancel and fill-or-kill remainders are dropped.
    const bool may_rest = incoming.type == OrderType::Limit ||
                          incoming.type == OrderType::Iceberg ||
                          incoming.type == OrderType::PostOnly;

    if (incoming.remaining > 0 && may_rest) {
        rest(incoming);
        result.resting = true;
    }

    if (incoming.remaining == 0) {
        result.status = OrderStatus::Filled;
        remember_finished(incoming, OrderStatus::Filled);
    } else if (result.resting) {
        result.status = result.trades.empty() ? OrderStatus::New : OrderStatus::PartiallyFilled;
    } else {
        // A market or immediate-or-cancel order that could not be completed.
        result.status = OrderStatus::Cancelled;
        remember_finished(incoming, OrderStatus::Cancelled);
    }

    return result;
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

    Order incoming;
    incoming.id                = next_order_id_++;
    incoming.side              = side;
    incoming.type              = type;
    incoming.price             = type == OrderType::Market ? kNoPrice : price;
    incoming.original_quantity = quantity;
    incoming.remaining         = quantity;
    incoming.visible           = quantity;
    incoming.display_size      = type == OrderType::Iceberg ? display_size : 0;

    return execute(incoming);
}

bool OrderBook::cancel(OrderId id) {
    if (locators_.find(id) == locators_.end()) {
        return false;  // unknown id, or the order is already fully filled
    }
    const Order taken = detach(id);
    remember_finished(taken, OrderStatus::Cancelled);
    return true;
}

ModifyResult OrderBook::modify(OrderId id, Price new_price, Quantity new_quantity) {
    if (new_quantity <= 0) {
        throw std::invalid_argument("modified quantity must be positive");
    }

    ModifyResult result;
    const auto   found = locators_.find(id);
    if (found == locators_.end()) {
        return result;  // found stays false
    }
    result.found = true;

    const Locator& locator       = found->second;
    Order&         resting_order = *locator.position;

    // Shrinking at the same price is the one change that keeps the order's
    // place in the queue. Everything else, including any size increase, is a
    // cancel and replace at the back of the queue.
    if (new_price == locator.price && new_quantity < resting_order.remaining) {
        const Quantity new_visible = resting_order.display_size > 0
                                         ? std::min(resting_order.visible, new_quantity)
                                         : new_quantity;

        auto shrink = [&](auto& ladder) {
            PriceLevel& level = ladder.at(locator.price);
            level.total_quantity   -= resting_order.remaining - new_quantity;
            level.visible_quantity -= resting_order.visible - new_visible;
        };
        if (locator.side == Side::Buy) {
            shrink(bids_);
        } else {
            shrink(asks_);
        }

        // The order keeps whatever it has already traded, so the original size
        // moves down with the remaining size.
        const Quantity already_filled =
            resting_order.original_quantity - resting_order.remaining;
        resting_order.remaining         = new_quantity;
        resting_order.visible           = new_visible;
        resting_order.original_quantity = already_filled + new_quantity;

        result.kept_queue_position = true;
        result.outcome.id          = id;
        result.outcome.resting     = true;
        result.outcome.status =
            already_filled > 0 ? OrderStatus::PartiallyFilled : OrderStatus::New;
        return result;
    }

    Order moved = detach(id);
    const Quantity already_filled = moved.original_quantity - moved.remaining;
    moved.price             = new_price;
    moved.remaining         = new_quantity;
    moved.visible           = new_quantity;  // rest() recomputes the iceberg slice
    moved.original_quantity = already_filled + new_quantity;

    result.outcome = execute(moved);
    return result;
}

std::optional<OrderState> OrderBook::order(OrderId id) const {
    if (const auto resting = locators_.find(id); resting != locators_.end()) {
        return state_of(*resting->second.position, true);
    }
    if (const auto finished = finished_.find(id); finished != finished_.end()) {
        return finished->second;
    }
    return std::nullopt;
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

std::optional<Price> OrderBook::mid_price() const {
    const auto bid = best_bid();
    const auto ask = best_ask();
    if (!bid || !ask) {
        return std::nullopt;
    }
    return (*bid + *ask) / 2;  // truncated to a whole tick
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

Quantity OrderBook::quantity_at_price(Side side, Price price) const {
    auto look_up = [&](const auto& ladder) -> Quantity {
        const auto level_it = ladder.find(price);
        return level_it == ladder.end() ? 0 : level_it->second.total_quantity;
    };
    return side == Side::Buy ? look_up(bids_) : look_up(asks_);
}

Quantity OrderBook::resting_quantity(Side side) const {
    Quantity total = 0;
    auto     add   = [&](const auto& ladder) {
        for (const auto& [level_price, level] : ladder) {
            (void)level_price;
            total += level.total_quantity;
        }
    };
    if (side == Side::Buy) {
        add(bids_);
    } else {
        add(asks_);
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

std::vector<std::string> OrderBook::validate() const {
    std::vector<std::string> problems;

    const auto bid = best_bid();
    const auto ask = best_ask();
    if (bid && ask && *bid >= *ask) {
        problems.push_back("book is crossed: best bid " + format_price(*bid) +
                           " is not below best ask " + format_price(*ask));
    }

    std::size_t counted_orders = 0;

    auto inspect = [&](const auto& ladder, Side expected_side) {
        for (const auto& [level_price, level] : ladder) {
            const std::string where =
                to_string(expected_side) + " level " + format_price(level_price);

            if (level.orders.empty()) {
                problems.push_back(where + " exists with no orders in it");
                continue;
            }

            Quantity visible_sum = 0;
            Quantity total_sum   = 0;
            for (const Order& resting : level.orders) {
                counted_orders += 1;
                visible_sum += resting.visible;
                total_sum   += resting.remaining;

                const std::string tag = where + " order " + std::to_string(resting.id);
                if (resting.remaining <= 0) {
                    problems.push_back(tag + " has no quantity left but is still resting");
                }
                if (resting.visible <= 0) {
                    problems.push_back(tag + " shows nothing but is still resting");
                }
                if (resting.visible > resting.remaining) {
                    problems.push_back(tag + " shows more than it has left");
                }
                if (resting.side != expected_side) {
                    problems.push_back(tag + " is on the wrong side of the book");
                }
                if (resting.price != level_price) {
                    problems.push_back(tag + " carries a different price than its level");
                }

                const auto locator = locators_.find(resting.id);
                if (locator == locators_.end()) {
                    problems.push_back(tag + " has no entry in the id lookup");
                } else if (locator->second.price != level_price ||
                           locator->second.side != expected_side ||
                           &(*locator->second.position) != &resting) {
                    problems.push_back(tag + " has a stale entry in the id lookup");
                }
            }

            if (visible_sum != level.visible_quantity) {
                problems.push_back(where + " cached visible size " +
                                   std::to_string(level.visible_quantity) +
                                   " does not match its orders " + std::to_string(visible_sum));
            }
            if (total_sum != level.total_quantity) {
                problems.push_back(where + " cached total size " +
                                   std::to_string(level.total_quantity) +
                                   " does not match its orders " + std::to_string(total_sum));
            }
        }
    };

    inspect(bids_, Side::Buy);
    inspect(asks_, Side::Sell);

    if (counted_orders != locators_.size()) {
        problems.push_back("id lookup holds " + std::to_string(locators_.size()) +
                           " orders but the book holds " + std::to_string(counted_orders));
    }

    return problems;
}

}  // namespace obe
