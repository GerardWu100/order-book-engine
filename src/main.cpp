// main.cpp - command line front end for the order book.
//
// Reads a session file (or standard input) where each line is one instruction,
// runs it against a single OrderBook, and prints what happened.
//
// Line formats, fields separated by spaces:
//
//   LIMIT    BUY|SELL  <price>  <quantity>
//   MARKET   BUY|SELL  -        <quantity>
//   IOC      BUY|SELL  <price>  <quantity>
//   FOK      BUY|SELL  <price>  <quantity>
//   POSTONLY BUY|SELL  <price>  <quantity>
//   ICEBERG  BUY|SELL  <price>  <quantity>  <display size>
//   CANCEL   <order id>
//   MODIFY   <order id>  <new price>  <new quantity>
//   STATUS   <order id>
//   BOOK
//   STATS
//
// Blank lines and lines starting with '#' are ignored.

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

#include "order.hpp"
#include "order_book.hpp"

namespace {

// How many price levels the BOOK command prints per side.
constexpr std::size_t kDefaultDepth = 5;

// Column width for the book display, wide enough for six digit sizes.
constexpr int kColumnWidth = 12;

obe::Side parse_side(const std::string& text) {
    if (text == "BUY") {
        return obe::Side::Buy;
    }
    if (text == "SELL") {
        return obe::Side::Sell;
    }
    throw std::invalid_argument("side must be BUY or SELL, got: " + text);
}

void print_trades(const obe::SubmitResult& result) {
    for (const obe::Trade& trade : result.trades) {
        std::cout << "  trade " << trade.quantity << " @ " << obe::format_price(trade.price)
                  << "  taker " << trade.taker_id << " maker " << trade.maker_id << "\n";
    }
}

void print_result(const obe::SubmitResult& result) {
    std::cout << "  order " << result.id << " " << obe::to_string(result.status) << "\n";
    print_trades(result);
    if (result.resting) {
        std::cout << "  remainder resting in the book\n";
    }
}

void print_book(const obe::OrderBook& book, std::size_t depth) {
    const auto bids = book.snapshot(obe::Side::Buy, depth);
    const auto asks = book.snapshot(obe::Side::Sell, depth);

    std::cout << "  " << std::setw(kColumnWidth) << "bid size" << std::setw(kColumnWidth) << "bid"
              << std::setw(kColumnWidth) << "ask" << std::setw(kColumnWidth) << "ask size" << "\n";

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

    if (const auto spread = book.spread()) {
        std::cout << "  spread " << obe::format_price(*spread) << ", mid "
                  << obe::format_price(*book.mid_price()) << "\n";
    }
    std::cout << "  resting orders " << book.resting_order_count() << "\n";
}

void print_statistics(const obe::OrderBook& book) {
    const obe::MarketStatistics& stats = book.statistics();
    std::cout << "  trades " << stats.trade_count << ", volume " << stats.traded_volume << "\n";
    if (stats.last_trade_price) {
        std::cout << "  last " << obe::format_price(*stats.last_trade_price) << ", high "
                  << obe::format_price(*stats.high_trade_price) << ", low "
                  << obe::format_price(*stats.low_trade_price) << ", vwap "
                  << obe::format_price(*stats.vwap()) << "\n";
    }
}

void print_status(const obe::OrderBook& book, obe::OrderId id) {
    const auto state = book.order(id);
    if (!state) {
        std::cout << "  no such order\n";
        return;
    }
    std::cout << "  " << obe::to_string(state->side) << " " << obe::to_string(state->type) << " "
              << obe::format_price(state->price) << " " << obe::to_string(state->status)
              << ", filled " << state->filled_quantity << " of " << state->original_quantity
              << ", left " << state->remaining_quantity
              << (state->resting ? ", resting\n" : ", not in the book\n");
}

// Run one instruction line. Unknown or malformed lines report an error and are
// skipped, so a typo does not abort a long session file.
void run_line(obe::OrderBook& book, const std::string& line, std::size_t depth) {
    std::istringstream fields(line);
    std::string        command;
    fields >> command;
    std::cout << line << "\n";

    if (command == "BOOK") {
        print_book(book, depth);
        return;
    }

    if (command == "STATS") {
        print_statistics(book);
        return;
    }

    if (command == "CANCEL") {
        obe::OrderId id = 0;
        fields >> id;
        std::cout << (book.cancel(id) ? "  cancelled\n" : "  cancel rejected, unknown order\n");
        return;
    }

    if (command == "STATUS") {
        obe::OrderId id = 0;
        fields >> id;
        print_status(book, id);
        return;
    }

    if (command == "MODIFY") {
        obe::OrderId  id = 0;
        std::string   price_text;
        obe::Quantity quantity = 0;
        fields >> id >> price_text >> quantity;

        const obe::ModifyResult result =
            book.modify(id, obe::parse_price(price_text), quantity);
        if (!result.found) {
            std::cout << "  amend rejected, order is not resting\n";
            return;
        }
        std::cout << (result.kept_queue_position ? "  size reduced, kept its place in the queue\n"
                                                 : "  requeued at the back of the price level\n");
        print_result(result.outcome);
        return;
    }

    obe::OrderType type;
    if (command == "LIMIT") {
        type = obe::OrderType::Limit;
    } else if (command == "MARKET") {
        type = obe::OrderType::Market;
    } else if (command == "IOC") {
        type = obe::OrderType::ImmediateOrCancel;
    } else if (command == "FOK") {
        type = obe::OrderType::FillOrKill;
    } else if (command == "ICEBERG") {
        type = obe::OrderType::Iceberg;
    } else if (command == "POSTONLY") {
        type = obe::OrderType::PostOnly;
    } else {
        std::cout << "  ignored, unknown command\n";
        return;
    }

    std::string   side_text, price_text;
    obe::Quantity quantity = 0;
    obe::Quantity display  = 0;
    fields >> side_text >> price_text >> quantity;
    if (type == obe::OrderType::Iceberg) {
        fields >> display;
    }

    const obe::Price price = price_text == "-" ? obe::kNoPrice : obe::parse_price(price_text);
    print_result(book.submit(parse_side(side_text), type, price, quantity, display));
}

}  // namespace

int main(int argc, char** argv) {
    const std::string path  = argc > 1 ? argv[1] : "-";
    const std::size_t depth = argc > 2 ? std::stoul(argv[2]) : kDefaultDepth;

    std::ifstream file;
    if (path != "-") {
        file.open(path);
        if (!file) {
            std::cerr << "cannot open session file: " << path << "\n";
            return 1;
        }
    }
    std::istream& input = path == "-" ? std::cin : file;

    obe::OrderBook book;
    std::string    line;
    while (std::getline(input, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        try {
            run_line(book, line, depth);
        } catch (const std::exception& error) {
            std::cout << "  rejected: " << error.what() << "\n";
        }
    }

    std::cout << "\nfinal book\n";
    print_book(book, depth);
    print_statistics(book);
    return 0;
}
