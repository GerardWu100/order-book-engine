// random_main.cpp - random order flow tester.
//
// Generates a stream of random orders, cancels and amendments, runs them
// through the book, checks the book's internal invariants along the way, and
// prints a summary.
//
// Usage:
//   ./build/order_book_random [instructions] [seed]
//
// Both arguments are optional. The same seed always replays the same session,
// so a failure can be reproduced exactly.
//
// Exit code 0 means the book stayed consistent and no quantity was lost or
// invented, so this can be used directly in a loop over many seeds.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <iomanip>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "order.hpp"
#include "order_book.hpp"
#include "random_flow.hpp"

namespace {

constexpr std::size_t kDefaultInstructions = 20000;
constexpr std::uint64_t kDefaultSeed       = 1;

// Price levels printed per side at the end.
constexpr std::size_t kDepth = 5;

// Drop ids that have left the book from the pool of cancel and amend targets
// every this many instructions. Without it the generator would spend most of
// its cancels on orders that are already gone.
constexpr std::size_t kCompactEvery = 500;

// Print a progress line this often.
constexpr std::size_t kProgressEvery = 5000;

constexpr int kLabelWidth  = 22;
constexpr int kColumnWidth = 12;

// Counters collected while the session runs.
struct SessionCounts {
    std::map<std::string, std::size_t> submitted_by_type;
    std::map<std::string, std::size_t> outcome_by_status;
    std::size_t cancels_accepted       = 0;
    std::size_t cancels_missed         = 0;  // the order was already gone
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

void print_book(const obe::OrderBook& book) {
    const auto bids = book.snapshot(obe::Side::Buy, kDepth);
    const auto asks = book.snapshot(obe::Side::Sell, kDepth);

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

// Drop ids that are no longer resting. Filled orders leave the book without
// telling us, so the pool goes stale on its own.
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
    obe::FlowSettings settings;
    settings.instruction_count =
        argc > 1 ? static_cast<std::size_t>(std::stoull(argv[1])) : kDefaultInstructions;
    settings.seed = argc > 2 ? std::stoull(argv[2]) : kDefaultSeed;

    obe::OrderBook  book;
    obe::RandomFlow flow(settings);

    SessionCounts             counts;
    std::vector<obe::OrderId> resting_ids;  // pool of ids to cancel or amend
    std::vector<obe::OrderId> every_id;     // every id ever created, for the audit below

    std::cout << "random order flow\n";
    print_row("instructions", static_cast<long long>(settings.instruction_count));
    print_row("seed", static_cast<long long>(settings.seed));
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

        // The book is checked after every single instruction, which is the
        // whole point of this tool.
        if (!book_is_healthy(book, step)) {
            return 1;
        }
        if (step % kCompactEvery == 0) {
            compact(book, resting_ids);
        }
        if (step % kProgressEvery == 0) {
            std::cout << "  " << step << " instructions, " << book.statistics().trade_count
                      << " trades, " << book.resting_order_count() << " resting\n";
        }
    }

    const auto   finished = std::chrono::steady_clock::now();
    const double seconds  = std::chrono::duration<double>(finished - started).count();

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
    print_book(book);
    if (const auto spread = book.spread()) {
        print_row("spread", obe::format_price(*spread));
    }
    print_row("resting orders", static_cast<long long>(book.resting_order_count()));

    std::cout << "\nchecks\n";
    print_row("invariants", "clean after every instruction");
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
