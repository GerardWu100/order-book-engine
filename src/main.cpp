// main.cpp - command line front end for the order book.
//
// Reads a session file (or standard input) where each line is one instruction,
// runs it against a single OrderBook, and prints what happened.
//
// Line formats, fields separated by spaces:
//
//   LIMIT   BUY|SELL  <price>  <quantity>
//   MARKET  BUY|SELL  -        <quantity>
//   IOC     BUY|SELL  <price>  <quantity>
//   FOK     BUY|SELL  <price>  <quantity>
//   ICEBERG BUY|SELL  <price>  <quantity>  <display size>
//   CANCEL  <order id>
//   BOOK
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
        std::cout << "  spread " << obe::format_price(*spread) << "\n";
    }
    std::cout << "  resting orders " << book.resting_order_count() << "\n";
}

// Run one instruction line. Unknown or malformed lines report an error and are
// skipped, so a typo does not abort a long session file.
void run_line(obe::OrderBook& book, const std::string& line, std::size_t depth) {
    std::istringstream fields(line);
    std::string        command;
    fields >> command;

    if (command == "BOOK") {
        std::cout << line << "\n";
        print_book(book, depth);
        return;
    }

    if (command == "CANCEL") {
        obe::OrderId id = 0;
        fields >> id;
        std::cout << line << "\n";
        std::cout << (book.cancel(id) ? "  cancelled\n" : "  cancel rejected, unknown order\n");
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
    } else {
        std::cout << "  ignored, unknown command: " << line << "\n";
        return;
    }

    std::string     side_text, price_text;
    obe::Quantity   quantity = 0;
    obe::Quantity   display  = 0;
    fields >> side_text >> price_text >> quantity;
    if (type == obe::OrderType::Iceberg) {
        fields >> display;
    }

    const obe::Price price =
        price_text == "-" ? obe::kNoPrice : obe::parse_price(price_text);

    const obe::SubmitResult result =
        book.submit(parse_side(side_text), type, price, quantity, display);

    std::cout << line << "\n";
    if (result.killed) {
        std::cout << "  order " << result.id
                  << " killed, not enough liquidity for the full size\n";
        return;
    }

    std::cout << "  order " << result.id << " accepted\n";
    print_trades(result);
    if (result.resting) {
        std::cout << "  remainder resting in the book\n";
    }
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
    return 0;
}
