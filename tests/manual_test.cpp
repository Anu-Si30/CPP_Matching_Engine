// =============================================================================
// manual_test.cpp  —  Interactive CLI to manually test the Matching Engine
//
// Build:
//   g++ -std=c++14 -O3 -Wall -Isrc
//       src/orderbook/order_book.cpp
//       src/matching/matching_engine.cpp
//       src/exchange/exchange.cpp
//       tests/manual_test.cpp
//       -o build/manual_test.exe
// =============================================================================

#include "orderbook/types.h"
#include "orderbook/order_book.h"
#include "exchange/exchange.h"

#include <iostream>
#include <string>
#include <sstream>
#include <vector>

#define RED    "\033[31m"
#define GREEN  "\033[32m"
#define YELLOW "\033[33m"
#define CYAN   "\033[36m"
#define BOLD   "\033[1m"
#define RESET  "\033[0m"

void print_book(const Exchange& ex, const std::string& sym) {
    const OrderBook* book = ex.get_book(sym);
    if (!book) {
        std::cout << "Symbol not found.\n";
        return;
    }

    auto asks = book->ask_depth(10);
    auto bids = book->bid_depth(10);

    std::cout << "\n" BOLD CYAN "=== ORDER BOOK: " << sym << " ===" RESET "\n";
    std::cout << "Mid Price: " << book->mid_price() << " | Spread: " << book->spread() << "\n\n";

    for (int i = (int)asks.size() - 1; i >= 0; i--) {
        std::cout << RED << "  $" << asks[i].first << "\t | \t" << asks[i].second << " shares (ASK)\n" << RESET;
    }
    std::cout << "-----------------------------------\n";
    for (size_t i = 0; i < bids.size(); i++) {
        std::cout << GREEN << "  $" << bids[i].first << "\t | \t" << bids[i].second << " shares (BID)\n" << RESET;
    }
    std::cout << "\n";
}

int main() {
    uint64_t order_id_counter = 1;

    Exchange ex([](const ExecutionReport& r) {
        std::cout << YELLOW BOLD "\n  [MATCH!] Executed " << r.exec_qty << " shares of " 
                  << r.symbol << " @ $" << r.exec_price << RESET "\n\n";
    });

    ex.add_symbol("AAPL");

    std::cout << BOLD CYAN "========================================================\n";
    std::cout << " MANUAL MATCHING ENGINE TEST\n";
    std::cout << "========================================================\n" RESET;
    std::cout << "Available commands:\n";
    std::cout << "  buy <qty> <price>      (e.g., buy 100 150.00)\n";
    std::cout << "  sell <qty> <price>     (e.g., sell 50 150.10)\n";
    std::cout << "  market buy <qty>       (e.g., market buy 10)\n";
    std::cout << "  market sell <qty>      (e.g., market sell 20)\n";
    std::cout << "  book                   (shows the order book)\n";
    std::cout << "  exit                   (quit)\n\n";

    std::string line;
    while (true) {
        std::cout << "> ";
        if (!std::getline(std::cin, line)) break;

        std::stringstream ss(line);
        std::string cmd;
        ss >> cmd;

        if (cmd == "exit" || cmd == "quit") {
            break;
        } else if (cmd == "book") {
            print_book(ex, "AAPL");
        } else if (cmd == "buy" || cmd == "sell") {
            uint32_t qty = 0;
            double price = 0.0;
            if (ss >> qty >> price) {
                auto o = global_order_pool.acquire();
                o->order_id = order_id_counter++;
                o->trader_id = 1;
                o->timestamp_ns = 1;
                o->side = (cmd == "buy") ? Side::BUY : Side::SELL;
                o->type = OrderType::LIMIT;
                o->price = price;
                o->quantity = qty;
                o->status = OrderStatus::NEW;
                o->set_symbol("AAPL");
                
                ex.submit_order(o);
                std::cout << "  -> Placed LIMIT " << cmd << " order for " << qty << " shares @ $" << price << "\n";
                print_book(ex, "AAPL");
            } else {
                std::cout << "Invalid format. Use: " << cmd << " <qty> <price>\n";
            }
        } else if (cmd == "market") {
            std::string side_str;
            uint32_t qty = 0;
            if (ss >> side_str >> qty && (side_str == "buy" || side_str == "sell")) {
                auto o = global_order_pool.acquire();
                o->order_id = order_id_counter++;
                o->trader_id = 1;
                o->timestamp_ns = 1;
                o->side = (side_str == "buy") ? Side::BUY : Side::SELL;
                o->type = OrderType::MARKET;
                o->price = 0.0;
                o->quantity = qty;
                o->status = OrderStatus::NEW;
                o->set_symbol("AAPL");

                ex.submit_order(o);
                std::cout << "  -> Placed MARKET " << side_str << " order for " << qty << " shares\n";
                print_book(ex, "AAPL");
            } else {
                std::cout << "Invalid format. Use: market buy <qty> OR market sell <qty>\n";
            }
        } else if (!cmd.empty()) {
            std::cout << "Unknown command.\n";
        }
    }

    return 0;
}
