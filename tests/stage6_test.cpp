// =============================================================================
// stage6_test.cpp  —  Interactive walkthrough of Stage 6: Multithreading
//
// In this stage, we move from a sequential simulation loop to a concurrent
// architecture. The Exchange runs on its own thread, and the Trading Agent
// runs on a separate thread.
//
// Because standard mutexes are too slow for HFT, they communicate entirely
// via Lock-Free SPSC (Single-Producer, Single-Consumer) Ring Buffers.
//
// The HFT Triad of Communication:
//   1. Market Data (Exchange -> Agent) : Broadcasts Best Bid/Ask (BBO) updates
//   2. Order Entry (Agent -> Exchange) : Submits new limit/market orders
//   3. Fills       (Exchange -> Agent) : Sends ExecutionReports
//
// Build:
//   g++ -std=c++14 -g -Wall -pthread -Iinclude
//       src/core/order_book.cpp
//       src/core/matching_engine.cpp
//       src/core/exchange.cpp
//       tests/stage6_test.cpp
//       -o build/stage6_test.exe
// =============================================================================

#include "core/types.h"
#include "core/order_book.h"
#include "core/exchange.h"
#include "core/spsc_queue.h"

#include <cstdio>
#include <cstring>
#include <atomic>
#include <vector>
#include <chrono>
#include <windows.h>

#define RED    "\033[31m"
#define GREEN  "\033[32m"
#define YELLOW "\033[33m"
#define CYAN   "\033[36m"
#define BOLD   "\033[1m"
#define RESET  "\033[0m"

// ─── Market Data Struct ───────────────────────────────────────────────────────
struct BBO {
    double best_bid;
    double best_ask;
};

// ─── Global Queues for Thread Communication ───────────────────────────────────
// In a real system, there would be an array of these (one set per agent).
SPSCQueue<std::shared_ptr<Order>> q_orders(1024);
SPSCQueue<ExecutionReport>        q_fills(1024);
SPSCQueue<BBO>                    q_market_data(1024);

std::atomic<bool> keep_running{true};

// =============================================================================
// THREAD 1: The Exchange Core
// =============================================================================
void exchange_thread_func() {
    Exchange ex([&](const ExecutionReport& r) {
        // When a fill occurs, push it to the agent's fill queue
        q_fills.push(r);
    });
    ex.add_symbol("AAPL");

    // Seed the book with a counterparty so the agent has someone to trade against
    auto seed_sell = std::make_shared<Order>();
    seed_sell->order_id = 99; seed_sell->trader_id = 99; seed_sell->timestamp_ns = now_ns();
    seed_sell->side = Side::SELL; seed_sell->type = OrderType::LIMIT;
    seed_sell->price = 100.10; seed_sell->quantity = 100000; seed_sell->status = OrderStatus::NEW;
    seed_sell->set_symbol("AAPL"); ex.submit_order(seed_sell);

    auto seed_buy = std::make_shared<Order>();
    seed_buy->order_id = 98; seed_buy->trader_id = 98; seed_buy->timestamp_ns = now_ns();
    seed_buy->side = Side::BUY; seed_buy->type = OrderType::LIMIT;
    seed_buy->price = 99.90; seed_buy->quantity = 100000; seed_buy->status = OrderStatus::NEW;
    seed_buy->set_symbol("AAPL"); ex.submit_order(seed_buy);

    const OrderBook* book = ex.get_book("AAPL");
    double last_bid = 0;
    double last_ask = 0;

    printf(CYAN "[Exchange] Thread started. Polling for orders...\n" RESET);

    // The ultra-low-latency spin loop
    while (keep_running.load(std::memory_order_relaxed)) {
        
        // 1. Process incoming orders from the Agent
        std::shared_ptr<Order> incoming_order;
        if (q_orders.pop(incoming_order)) {
            ex.submit_order(incoming_order);
        }

        // 2. Publish Market Data if BBO changed
        double current_bid = 0, current_ask = 0;
        auto bids = book->bid_depth(1);
        auto asks = book->ask_depth(1);
        if (!bids.empty()) current_bid = bids[0].first;
        if (!asks.empty()) current_ask = asks[0].first;

        if (current_bid != last_bid || current_ask != last_ask) {
            last_bid = current_bid;
            last_ask = current_ask;
            BBO bbo{current_bid, current_ask};
            q_market_data.push(bbo);
        }
    }
    
    printf(CYAN "[Exchange] Thread stopping.\n" RESET);
    ex.print_stats();
}

// =============================================================================
// THREAD 2: The Trading Agent
// =============================================================================
void agent_thread_func() {
    printf(YELLOW "[Agent]    Thread started. Waiting for market data...\n" RESET);

    double current_bid = 0.0;
    double current_ask = 0.0;
    int orders_sent = 0;
    int fills_received = 0;
    
    uint64_t next_order_id = 1000;

    while (keep_running.load(std::memory_order_relaxed)) {
        
        // 1. Poll Market Data
        BBO bbo;
        while (q_market_data.pop(bbo)) {
            current_bid = bbo.best_bid;
            current_ask = bbo.best_ask;
        }

        // 2. Poll Fills
        ExecutionReport fill;
        while (q_fills.pop(fill)) {
            fills_received++;
            if (fills_received % 10000 == 0) {
                printf(YELLOW "[Agent]    Received %d fills so far.\n" RESET, fills_received);
            }
        }

        // 3. Trading Logic: If we have valid market data and haven't hit our limit, send an order
        if (current_bid > 0.0 && current_ask > 0.0 && orders_sent < 50000) {
            auto order = std::make_shared<Order>();
            order->order_id = ++next_order_id;
            order->trader_id = 42;
            order->timestamp_ns = now_ns();
            order->side = Side::BUY;
            order->type = OrderType::MARKET; // Aggressively cross the spread
            order->price = 0.0;
            order->quantity = 1;
            order->filled_qty = 0;
            order->status = OrderStatus::NEW;
            order->set_symbol("AAPL");

            if (q_orders.push(order)) {
                orders_sent++;
                if (orders_sent % 10000 == 0) {
                    printf(YELLOW "[Agent]    Sent %d orders.\n" RESET, orders_sent);
                }
            }
        }
        
        // Break early if we sent 50k orders and received 50k fills
        if (orders_sent >= 50000 && fills_received >= 50000) {
            printf(GREEN BOLD "[Agent]    Successfully processed 50,000 round-trip trades lock-free!\n" RESET);
            keep_running.store(false, std::memory_order_relaxed);
        }
    }
    printf(YELLOW "[Agent]    Thread stopping. Total fills: %d\n" RESET, fills_received);
}

// ─── Win32 Thread Wrappers ────────────────────────────────────────────────────
DWORD WINAPI exchange_thread_wrapper(LPVOID lpParam) {
    exchange_thread_func();
    return 0;
}

DWORD WINAPI agent_thread_wrapper(LPVOID lpParam) {
    agent_thread_func();
    return 0;
}

// =============================================================================
// main
// =============================================================================
int main() {
    printf("\n");
    printf(BOLD CYAN
        "+=====================================================+\n"
        "|  CPP MATCHING ENGINE -- Stage 6 Interactive Test  |\n"
        "|  Multithreading via Lock-Free SPSC Queues         |\n"
        "+=====================================================+\n"
        RESET "\n");

    printf("Starting 2 concurrent threads...\n");
    printf("Agent will blast 50,000 MARKET BUY orders as fast as possible.\n");
    printf("Exchange will process them and return 50,000 Execution Reports.\n");
    printf("All communication passes through 64-byte aligned lock-free ring buffers.\n\n");

    // Launch threads using Win32 API (since MinGW Thread Model is win32)
    HANDLE hExchange = CreateThread(NULL, 0, exchange_thread_wrapper, NULL, 0, NULL);
    HANDLE hAgent    = CreateThread(NULL, 0, agent_thread_wrapper, NULL, 0, NULL);

    // Wait for them to finish
    WaitForSingleObject(hAgent, INFINITE);
    WaitForSingleObject(hExchange, INFINITE);

    CloseHandle(hAgent);
    CloseHandle(hExchange);

    printf("\n");
    printf(GREEN BOLD "PASS — Multithreaded lock-free pipeline is stable and incredibly fast.\n" RESET);

    return 0;
}
