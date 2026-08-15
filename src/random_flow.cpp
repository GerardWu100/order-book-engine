// random_flow.cpp - the random order generator declared in random_flow.hpp.

#include "random_flow.hpp"

#include <algorithm>

namespace obe {

RandomFlow::RandomFlow(const FlowSettings& settings)
    : settings_(settings),
      engine_(settings.seed),
      type_picker_(settings.type_weights.begin(), settings.type_weights.end()),
      quantity_picker_(settings.min_quantity, settings.max_quantity),
      offset_picker_(-settings.price_band_ticks, settings.price_band_ticks),
      chance_(0.0, 1.0) {}

Price RandomFlow::reference_price(const OrderBook& book) const {
    if (const auto mid = book.mid_price()) {
        return *mid;
    }
    if (const auto bid = book.best_bid()) {
        return *bid;
    }
    if (const auto ask = book.best_ask()) {
        return *ask;
    }
    return settings_.starting_mid_price;
}

Price RandomFlow::random_price_near(Price reference) {
    const Price price = reference + offset_picker_(engine_);
    return std::max<Price>(price, 1);  // never quote at or below zero
}

Quantity RandomFlow::random_quantity() {
    return quantity_picker_(engine_);
}

OrderType RandomFlow::random_type() {
    return static_cast<OrderType>(type_picker_(engine_));
}

Side RandomFlow::random_side() {
    return chance_(engine_) < 0.5 ? Side::Buy : Side::Sell;
}

Instruction RandomFlow::next(const OrderBook& book, const std::vector<OrderId>& resting_ids) {
    Instruction instruction;
    const Price reference = reference_price(book);

    // Cancels and amendments need something to act on. With an empty book the
    // generator falls through to a submit.
    if (!resting_ids.empty()) {
        const double roll = chance_(engine_);
        std::uniform_int_distribution<std::size_t> pick(0, resting_ids.size() - 1);

        if (roll < settings_.cancel_share) {
            instruction.kind   = Instruction::Kind::Cancel;
            instruction.target = resting_ids[pick(engine_)];
            return instruction;
        }
        if (roll < settings_.cancel_share + settings_.modify_share) {
            instruction.kind   = Instruction::Kind::Modify;
            instruction.target = resting_ids[pick(engine_)];

            // Half the amendments (by default) keep the price and cut the size,
            // which is the case that keeps its place in the queue. The rest
            // move the price, which sends the order to the back.
            const auto  current = book.order(instruction.target);
            const bool  shrink  = chance_(engine_) < settings_.modify_shrink_share;
            if (shrink && current && current->resting && current->remaining_quantity > 1) {
                std::uniform_int_distribution<Quantity> smaller(
                    1, current->remaining_quantity - 1);
                instruction.new_price    = current->price;
                instruction.new_quantity = smaller(engine_);
            } else {
                instruction.new_price    = random_price_near(reference);
                instruction.new_quantity = random_quantity();
            }
            return instruction;
        }
    }

    instruction.kind     = Instruction::Kind::Submit;
    instruction.side     = random_side();
    instruction.type     = random_type();
    instruction.quantity = random_quantity();
    instruction.price    = instruction.type == OrderType::Market
                               ? kNoPrice
                               : random_price_near(reference);

    if (instruction.type == OrderType::Iceberg) {
        // At least one unit shown, so the order is never invisible.
        instruction.display_size =
            std::max<Quantity>(1, instruction.quantity / settings_.iceberg_display_divisor);
    }
    return instruction;
}

}  // namespace obe
