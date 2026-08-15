// random_flow.hpp
//
// A random order generator for testing the book.
//
// It produces a stream of instructions (submit, cancel, modify) that look
// roughly like real order flow: prices scattered around the current midpoint,
// a mix of order types, and a steady trickle of cancels and amendments against
// orders that are already resting.
//
// The stream is fully determined by the seed. The same seed replays the same
// session, which is what makes a failure reproducible: note the seed, fix the
// bug, rerun the same seed.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

#include "order.hpp"
#include "order_book.hpp"

namespace obe {

// Everything that shapes the generated flow. The defaults produce a busy two
// sided book around 100.00; the two that usually matter, the seed and the
// number of instructions, are set from the command line.
struct FlowSettings {
    std::uint64_t seed              = 1;
    std::size_t   instruction_count = 20000;

    // Midpoint used only while the book is empty. Once there are prices on
    // both sides, orders are placed around the live midpoint instead.
    Price starting_mid_price = 100 * kTicksPerUnit;

    // How far from the midpoint a generated price can land, in ticks.
    Price price_band_ticks = 20;

    Quantity min_quantity = 100;
    Quantity max_quantity = 1000;

    // Iceberg display size = quantity / this, so 4 shows a quarter.
    std::int64_t iceberg_display_divisor = 4;

    double cancel_share = 0.15;  // share of instructions that cancel
    double modify_share = 0.10;  // share that amend a resting order

    // Share of amendments that keep the price and only shrink the order. That
    // is the one amendment that keeps its place in the queue, so without this
    // the generator would almost never exercise that path: a random new price
    // is hardly ever equal to the old one.
    double modify_shrink_share = 0.50;

    // One weight per OrderType, indexed by the enum value. Relative, not
    // percentages; the generator normalises them.
    std::array<double, kOrderTypeCount> type_weights{{60, 8, 8, 4, 8, 12}};
};

// One generated instruction. `kind` says which fields matter.
struct Instruction {
    enum class Kind { Submit, Cancel, Modify };

    Kind kind = Kind::Submit;

    // Submit
    Side      side         = Side::Buy;
    OrderType type         = OrderType::Limit;
    Price     price        = kNoPrice;
    Quantity  quantity     = 0;
    Quantity  display_size = 0;

    // Cancel and Modify
    OrderId  target       = 0;
    Price    new_price    = kNoPrice;
    Quantity new_quantity = 0;
};

class RandomFlow {
public:
    explicit RandomFlow(const FlowSettings& settings);

    // Produce the next instruction.
    //
    // `book` is read only, to anchor prices on the current midpoint.
    // `resting_ids` are the orders currently in the book; when it is empty the
    // generator always submits, because there is nothing to cancel or amend.
    Instruction next(const OrderBook& book, const std::vector<OrderId>& resting_ids);

    const FlowSettings& settings() const { return settings_; }

private:
    // Current midpoint if both sides have prices, the best price on whichever
    // side exists, otherwise the configured starting midpoint.
    Price reference_price(const OrderBook& book) const;

    Price     random_price_near(Price reference);
    Quantity  random_quantity();
    OrderType random_type();
    Side      random_side();

    FlowSettings                            settings_;
    std::mt19937_64                         engine_;
    std::discrete_distribution<std::size_t> type_picker_;
    std::uniform_int_distribution<Quantity> quantity_picker_;
    std::uniform_int_distribution<Price>    offset_picker_;
    std::uniform_real_distribution<double>  chance_;
};

}  // namespace obe
