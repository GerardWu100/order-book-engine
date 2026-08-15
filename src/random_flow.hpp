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

#include "config.hpp"
#include "order.hpp"
#include "order_book.hpp"

namespace obe {

// Everything that shapes the generated flow. Read from config.toml so the mix
// can be retuned without recompiling.
struct FlowSettings {
    std::uint64_t seed                    = 1;
    std::size_t   instruction_count       = 5000;
    Price         starting_mid_price      = 100 * kTicksPerUnit;
    Price         price_band_ticks        = 20;   // how far from the midpoint prices land
    Quantity      min_quantity            = 100;
    Quantity      max_quantity            = 1000;
    std::int64_t  iceberg_display_divisor = 4;    // display size = quantity / this
    double        cancel_share            = 0.15; // share of instructions that cancel
    double        modify_share            = 0.10; // share that amend a resting order

    // Share of amendments that keep the price and only shrink the order. That
    // is the one amendment that keeps its place in the queue, so without this
    // the generator would almost never exercise that path: a random new price
    // is hardly ever equal to the old one.
    double        modify_shrink_share     = 0.50;

    // One weight per OrderType, indexed by the enum value. Relative, not
    // percentages; the generator normalises them.
    std::array<double, kOrderTypeCount> type_weights{{60, 8, 8, 4, 8, 12}};

    // Read every field from a Config. Throws if a key is missing.
    static FlowSettings from_config(const Config& config);
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
