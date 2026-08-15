// random_main.cpp - random order flow tester.
//
// Generates a stream of random orders, cancels and amendments from config.toml,
// runs them through the book, checks the book's internal invariants along the
// way, and prints a summary.
//
// Usage:
//   ./build/order_book_random [config file] [seed override]
//
// The seed override is there so you can sweep many sessions from a shell loop
// without editing the config file.

#include <chrono>
#include <cstddef>
#include <iomanip>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "config.hpp"
#include "order.hpp"
#include "order_book.hpp"
#include "random_flow.hpp"

namespace {

constexpr int kLabelWidth  = 22;
constexpr int kColumnWidth = 12;

// Counters collected while the session runs.
struct SessionCounts {
    std::map<std::string, std::size_t> submitted_by_type;
    std::map<std::string, std::size_t> outcome_by_status;
    std::size_t cancels_accepted = 0;
    std::size_t cancels_missed   = 0;  // the order was already gone
    std::size_t modifies_kept_priority = 0;
    std::size_t modifies_requeued      = 0;
    std::size_t modifies_missed        = 0;
};

void print_row(const std::string& label, const std::string& value) {
    std::cout << "  " << std::left << std::setw(kLabelWidth) << label << std::right << value
              << "\n";
}

void print_row(const std::string& label, long long value) {
    print_row(label, std::to_string(value));
}

void print_book(const obe::OrderBook& book, std::size_t depth) {
    const auto bids = book.snapshot(obe::Side::Buy, depth);
    const auto asks = book.snapshot(obe::Side::Sell, depth);

    std::cout << "  " << std::right << std::setw(kColumnWidth) << "bid size"
              << std::setw(kColumnWidth) << "bid" << std::setw(kColumnWidth) << "ask"
              << std::setw(kColumnWidth) << "ask size" << "\n";

    const std::size_t rows = std::max(bids.size(), asks.size());
    for (std::size_t row = 0; row < rows; ++row) {
        std::string bid_size, bid_price, ask_price, ask_size;
        if (row < bids.size()) {
            bid_size  = std::to_string(bids[row].visible_quantity);
            bid_price = obe::format_price(bids[row].price);
        }
        if (row < asks.size()) {
            ask_price = obe::format_price(asks[row].price);
            ask_size  = std::to_string(asks[row].visible_quantity);
        }
        std::cout << "  " << std::setw(kColumnWidth) << bid_size << std::setw(kColumnWidth)
                  << bid_price << std::setw(kColumnWidth) << ask_price << std::setw(kColumnWidth)
                  << ask_size << "\n";
    }
}

// Drop ids that are no longer resting, so the pool the generator picks from
// stays mostly live. Filled orders leave the book without telling us.
void compact(const obe::OrderBook& book, std::vector<obe::OrderId>& resting_ids) {
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

// Report any invariant breach and stop the session immediately, because
// carrying on would only pile more damage on top of the first fault.
bool book_is_healthy(const obe::OrderBook& book, std::size_t step) {
    const auto problems = book.validate();
    if (problems.empty()) {
        return true;
    }
    std::cout << "\nbook inconsistent after instruction " << step << ":\n";
    for (const std::string& problem : problems) {
        std::cout << "  " << problem << "\n";
    }
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string config_path = argc > 1 ? argv[1] : "config.toml";

    obe::Config config;
    try {
        config = obe::Config::from_file(config_path);
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
        std::cerr << "run this from the project root, or pass the path to config.toml\n";
        return 1;
    }

    obe::FlowSettings settings;
    std::size_t       depth = 0, validate_every = 0, compact_every = 0, progress_every = 0;
    try {
        settings       = obe::FlowSettings::from_config(config);
        depth          = static_cast<std::size_t>(config.integer("report.depth"));
        validate_every = static_cast<std::size_t>(config.integer("report.validate_every"));
        compact_every  = static_cast<std::size_t>(config.integer("report.compact_every"));
        progress_every = static_cast<std::size_t>(config.integer("report.progress_every"));
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
        return 1;
    }

    if (argc > 2) {
        settings.seed = std::stoull(argv[2]);
    }

    obe::OrderBook book;
    obe::RandomFlow flow(settings);

    SessionCounts             counts;
    std::vector<obe::OrderId> resting_ids;  // pool of ids to cancel or amend
    std::vector<obe::OrderId> every_id;     // every id ever created, for the audit below

    std::cout << "random order flow\n";
    print_row("seed", static_cast<long long>(settings.seed));
    print_row("instructions", static_cast<long long>(settings.instruction_count));
    print_row("starting mid", obe::format_price(settings.starting_mid_price));
    std::cout << "\n";

    const auto started = std::chrono::steady_clock::now();

    for (std::size_t step = 1; step <= settings.instruction_count; ++step) {
        const obe::Instruction instruction = flow.next(book, resting_ids);

        switch (instruction.kind) {
            case obe::Instruction::Kind::Submit: {
                const obe::SubmitResult result =
                    book.submit(instruction.side, instruction.type, instruction.price,
                                instruction.quantity, instruction.display_size);
                counts.submitted_by_type[obe::to_string(instruction.type)] += 1;
                counts.outcome_by_status[obe::to_string(result.status)] += 1;
                every_id.push_back(result.id);
                if (result.resting) {
                    resting_ids.push_back(result.id);
                }
                break;
            }
            case obe::Instruction::Kind::Cancel: {
                if (book.cancel(instruction.target)) {
                    counts.cancels_accepted += 1;
                } else {
                    counts.cancels_missed += 1;
                }
                break;
            }
            case obe::Instruction::Kind::Modify: {
                const obe::ModifyResult result = book.modify(
                    instruction.target, instruction.new_price, instruction.new_quantity);
                if (!result.found) {
                    counts.modifies_missed += 1;
                } else if (result.kept_queue_position) {
                    counts.modifies_kept_priority += 1;
                } else {
                    counts.modifies_requeued += 1;
                    if (result.outcome.resting) {
                        resting_ids.push_back(result.outcome.id);
                    }
                }
                break;
            }
        }

        if (validate_every > 0 && step % validate_every == 0 && !book_is_healthy(book, step)) {
            return 1;
        }
        if (compact_every > 0 && step % compact_every == 0) {
            compact(book, resting_ids);
        }
        if (progress_every > 0 && step % progress_every == 0) {
            std::cout << "  " << step << " instructions, " << book.statistics().trade_count
                      << " trades, " << book.resting_order_count() << " resting\n";
        }
    }

    const auto finished = std::chrono::steady_clock::now();
    const double seconds =
        std::chrono::duration<double>(finished - started).count();

    if (!book_is_healthy(book, settings.instruction_count)) {
        return 1;
    }

    // Conservation check. Every execution adds the same quantity to a buyer and
    // a seller, so the filled quantity summed over all orders must be exactly
    // twice the traded volume. If the engine ever loses or invents quantity,
    // this is where it shows up.
    obe::Quantity filled_across_all_orders = 0;
    for (const obe::OrderId id : every_id) {
        if (const auto state = book.order(id)) {
            filled_across_all_orders += state->filled_quantity;
        }
    }
    const obe::Quantity expected = 2 * book.statistics().traded_volume;

    const obe::MarketStatistics& stats = book.statistics();

    std::cout << "\nsubmitted orders\n";
    for (const auto& [type, count] : counts.submitted_by_type) {
        print_row(type, static_cast<long long>(count));
    }

    std::cout << "\noutcomes\n";
    for (const auto& [status, count] : counts.outcome_by_status) {
        print_row(status, static_cast<long long>(count));
    }

    std::cout << "\ncancels and amendments\n";
    print_row("cancelled", static_cast<long long>(counts.cancels_accepted));
    print_row("cancel too late", static_cast<long long>(counts.cancels_missed));
    print_row("amend kept place", static_cast<long long>(counts.modifies_kept_priority));
    print_row("amend requeued", static_cast<long long>(counts.modifies_requeued));
    print_row("amend too late", static_cast<long long>(counts.modifies_missed));

    std::cout << "\ntrading\n";
    print_row("trades", static_cast<long long>(stats.trade_count));
    print_row("volume", static_cast<long long>(stats.traded_volume));
    if (stats.last_trade_price) {
        print_row("last price", obe::format_price(*stats.last_trade_price));
        print_row("high", obe::format_price(*stats.high_trade_price));
        print_row("low", obe::format_price(*stats.low_trade_price));
    }

    std::cout << "\nfinal book\n";
    print_book(book, depth);
    if (const auto spread = book.spread()) {
        print_row("spread", obe::format_price(*spread));
    }
    print_row("resting orders", static_cast<long long>(book.resting_order_count()));
    print_row("bid quantity", static_cast<long long>(book.resting_quantity(obe::Side::Buy)));
    print_row("ask quantity", static_cast<long long>(book.resting_quantity(obe::Side::Sell)));

    std::cout << "\nchecks\n";
    print_row("invariants", "clean");
    print_row("quantity conserved",
              filled_across_all_orders == expected
                  ? "yes"
                  : "NO (" + std::to_string(filled_across_all_orders) + " vs " +
                        std::to_string(expected) + ")");
    std::cout << "  " << std::left << std::setw(kLabelWidth) << "speed" << std::right
              << std::fixed << std::setprecision(0)
              << (seconds > 0 ? settings.instruction_count / seconds : 0)
              << " instructions/sec\n";

    return filled_across_all_orders == expected ? 0 : 1;
}
