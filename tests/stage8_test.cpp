// =============================================================================
// stage8_test.cpp  —  Interactive walkthrough of Stage 8: Terminal UI
//
// Build:
//   g++ -std=c++14 -O3 -Wall -Isrc
//       src/orderbook/order_book.cpp
//       src/matching/matching_engine.cpp
//       src/exchange/exchange.cpp
//       tests/stage8_test.cpp
//       -o build/stage8_test.exe
// =============================================================================

#include "orderbook/types.h"
#include "orderbook/order_book.h"
#include "exchange/exchange.h"
#include "traders/random_trader.h"
#include "traders/market_maker.h"

#include <cstdio>
#include <cstring>
#include <vector>
#include <string>
#include <windows.h>

#define RED    "\033[31m"
#define GREEN  "\033[32m"
#define YELLOW "\033[33m"
#define CYAN   "\033[36m"
#define BOLD   "\033[1m"
#define RESET  "\033[0m"

// Clears the terminal screen and moves cursor to top-left
void clear_screen() {
    printf("\033[2J\033[H");
}

std::string format_price(double price) {
    char buf[32];
    snprintf(buf, sizeof(buf), "$%.2f", price);
    return std::string(buf);
}

void print_dashboard(const Exchange& ex, MarketMaker& mm, RandomTrader& rand) {
    const OrderBook* book = ex.get_book("AAPL");
    if (!book) return;

    double mid = book->mid_price();
    
    clear_screen();
    
    printf(BOLD CYAN "+========================================================================+\n");
    printf("|  CPP MATCHING ENGINE -- LIVE DASHBOARD                                 |\n");
    printf("+========================================================================+\n" RESET);
    
    printf("|  SYMBOL: AAPL    |  MID: %-8s |  SPREAD: $%-8.2f                 |\n", 
           format_price(mid).c_str(), book->spread());
    printf(BOLD CYAN "+-------------------------+----------------------------------------------+\n" RESET);
    printf("|       ORDER BOOK        |             TRADING AGENTS                   |\n");
    printf("|                         |                                              |\n");

    auto asks = book->ask_depth(5);
    auto bids = book->bid_depth(5);

    // We want to print 11 rows total for the split view
    for (int i = 0; i < 11; i++) {
        std::string left_col  = "                        "; // 24 spaces
        std::string right_col = "                                            "; // 44 spaces

        // --- Build Left Column (Order Book) ---
        if (i < 5) {
            // Asks (print in reverse order so lowest ask is at bottom)
            int ask_idx = 4 - i;
            if (ask_idx < (int)asks.size()) {
                char buf[64];
                snprintf(buf, sizeof(buf), RED "  %-7s | %4lu ASK  " RESET, 
                         format_price(asks[ask_idx].first).c_str(), (unsigned long)asks[ask_idx].second);
                left_col = buf;
            }
        } else if (i == 5) {
            left_col = "  --------------------- ";
        } else {
            // Bids
            int bid_idx = i - 6;
            if (bid_idx < (int)bids.size()) {
                char buf[64];
                snprintf(buf, sizeof(buf), GREEN "  %-7s | %4lu BID  " RESET, 
                         format_price(bids[bid_idx].first).c_str(), (unsigned long)bids[bid_idx].second);
                left_col = buf;
            }
        }

        // --- Build Right Column (Agents) ---
        if (i == 1) {
            right_col = BOLD "  [Market Maker]" RESET "                              ";
        } else if (i == 2) {
            char buf[64];
            snprintf(buf, sizeof(buf), "  Inventory: %+5ld shares                     ", (long)mm.inventory());
            right_col = buf;
        } else if (i == 3) {
            char buf[64];
            snprintf(buf, sizeof(buf), "  Fills:     %-6lu                           ", (unsigned long)mm.fill_count());
            right_col = buf;
        } else if (i == 4) {
            char buf[64];
            double pnl = mm.total_pnl(mid);
            const char* color = pnl >= 0 ? GREEN : RED;
            snprintf(buf, sizeof(buf), "  Total PnL: %s%+8.2f" RESET "                           ", color, pnl);
            right_col = buf;
        } else if (i == 6) {
            right_col = BOLD "  [Noise Trader]" RESET "                              ";
        } else if (i == 7) {
            char buf[64];
            snprintf(buf, sizeof(buf), "  Inventory: %+5ld shares                     ", (long)rand.inventory());
            right_col = buf;
        } else if (i == 8) {
            char buf[64];
            snprintf(buf, sizeof(buf), "  Fills:     %-6lu                           ", (unsigned long)rand.fill_count());
            right_col = buf;
        } else if (i == 9) {
            char buf[64];
            double pnl = rand.total_pnl(mid);
            const char* color = pnl >= 0 ? GREEN : RED;
            snprintf(buf, sizeof(buf), "  Total PnL: %s%+8.2f" RESET "                           ", color, pnl);
            right_col = buf;
        }

        printf("| %s|%s|\n", left_col.c_str(), right_col.c_str());
    }

    printf(BOLD CYAN "+========================================================================+\n" RESET);
}

#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif

int main() {
    // Enable virtual terminal processing for Windows console (to parse ANSI escape codes)
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD dwMode = 0;
    GetConsoleMode(hOut, &dwMode);
    SetConsoleMode(hOut, dwMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);

    RandomTrader rand(1, 1337); rand.set_symbol("AAPL");
    MarketMaker  mm(2, nullptr); mm.set_symbol("AAPL"); 

    std::vector<TradingAgent*> agents = { &rand, &mm };

    Exchange ex([&](const ExecutionReport& r) {
        for (TradingAgent* a : agents)
            if (r.trader_id == a->trader_id) a->on_fill(r);
    });
    ex.add_symbol("AAPL");
    
    MarketMaker mm_actual(2, &ex);
    mm_actual.set_symbol("AAPL");
    agents[1] = &mm_actual;

    // Seed book 
    auto seed_sell = global_order_pool.acquire();
    seed_sell->order_id = 99; seed_sell->trader_id = 99; seed_sell->timestamp_ns = 1;
    seed_sell->side = Side::SELL; seed_sell->type = OrderType::LIMIT;
    seed_sell->price = 100.10; seed_sell->quantity = 1000; seed_sell->status = OrderStatus::NEW;
    seed_sell->set_symbol("AAPL"); ex.submit_order(seed_sell);

    auto seed_buy = global_order_pool.acquire();
    seed_buy->order_id = 98; seed_buy->trader_id = 98; seed_buy->timestamp_ns = 1;
    seed_buy->side = Side::BUY; seed_buy->type = OrderType::LIMIT;
    seed_buy->price = 99.90; seed_buy->quantity = 1000; seed_buy->status = OrderStatus::NEW;
    seed_buy->set_symbol("AAPL"); ex.submit_order(seed_buy);

    const OrderBook* book = ex.get_book("AAPL");
    
    // Run simulation loop at 10 FPS for 10 seconds (100 ticks)
    for (int tick = 0; tick < 200; tick++) {
        uint64_t ts = tick + 10;
        
        for (TradingAgent* a : agents) {
            auto orders = a->on_tick(*book, ts);
            for (auto& o : orders) ex.submit_order(o);
        }

        print_dashboard(ex, mm_actual, rand);
        
        // Artificial delay so human eyes can watch the UI update
        Sleep(50);
    }

    printf("\nSimulation complete.\n");
    return 0;
}
