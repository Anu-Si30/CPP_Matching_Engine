// =============================================================================
// stage7_test.cpp  —  Interactive walkthrough of Stage 7: Benchmarking
//
// In low-latency trading, average latency is meaningless. You care about the
// tail percentiles (p99, p99.9). If your system is fast 99% of the time but
// stalls for 5 milliseconds 1% of the time, your algorithm will lose money
// during those spikes.
//
// This test fires 1,000,000 orders through the lock-free SPSC queues and the
// matching engine, and measures:
//   1. Throughput (Orders per second)
//   2. Latency percentiles (Time from order creation to fill receipt)
//
// Build:
//   g++ -std=c++14 -O3 -Wall -Iinclude
//       src/core/order_book.cpp
//       src/core/matching_engine.cpp
//       src/core/exchange.cpp
//       tests/stage7_test.cpp
//       -o build/stage7_test.exe
// =============================================================================

#include "core/types.h"
#include "core/order_book.h"
#include "core/exchange.h"
#include "core/spsc_queue.h"

#include <cstdio>
#include <cstring>
#include <vector>
#include <chrono>
#include <algorithm>
#include <windows.h>

#define RED    "\033[31m"
#define GREEN  "\033[32m"
#define YELLOW "\033[33m"
#define CYAN   "\033[36m"
#define BOLD   "\033[1m"
#define RESET  "\033[0m"

const size_t NUM_ORDERS = 100000;
const size_t QUEUE_SIZE = 8192; // Slightly larger queue to handle bursty throughput

SPSCQueue<std::shared_ptr<Order>> q_orders(QUEUE_SIZE);
SPSCQueue<ExecutionReport>        q_fills(QUEUE_SIZE);
std::atomic<bool> keep_running{true};

// Arrays for measuring latency. Pre-allocated to avoid OS memory jitter during the benchmark.
std::vector<uint64_t> t_send(NUM_ORDERS, 0);
std::vector<uint64_t> latencies(NUM_ORDERS, 0);
std::vector<std::shared_ptr<Order>> preallocated_orders;

// =============================================================================
// THREAD 1: The Exchange Core
// =============================================================================
DWORD WINAPI exchange_thread_func(LPVOID) {
    Exchange ex([](const ExecutionReport& r) {
        if (r.trader_id == 42) {
            // We spin if the fill queue is full (backpressure)
            while (!q_fills.push(r)) {
                if (!keep_running.load(std::memory_order_relaxed)) return;
            }
        }
    });
    ex.add_symbol("AAPL");

    // Seed the book with a massive counterparty so we never run out of shares to match against
    auto seed_sell = std::make_shared<Order>();
    seed_sell->order_id = 9999999; seed_sell->trader_id = 99; seed_sell->timestamp_ns = now_ns();
    seed_sell->side = Side::SELL; seed_sell->type = OrderType::LIMIT;
    seed_sell->price = 100.10; seed_sell->quantity = NUM_ORDERS; seed_sell->status = OrderStatus::NEW;
    seed_sell->set_symbol("AAPL"); ex.submit_order(seed_sell);

    while (keep_running.load(std::memory_order_relaxed)) {
        std::shared_ptr<Order> incoming_order;
        if (q_orders.pop(incoming_order)) {
            ex.submit_order(incoming_order);
        }
    }
    return 0;
}

// =============================================================================
// THREAD 2: The Benchmark Harness (Agent)
// =============================================================================
DWORD WINAPI agent_thread_func(LPVOID) {
    size_t orders_sent = 0;
    size_t fills_received = 0;

    // Wait a brief moment to ensure the exchange thread is fully spun up and seeded
    Sleep(50); 
    
    printf(CYAN "[Benchmark] Pumping %lu orders...\n" RESET, (unsigned long)NUM_ORDERS);

    uint64_t start_time = now_ns();

    while (orders_sent < NUM_ORDERS) {
        // 1. Process any pending fills to prevent deadlock (backpressure relief)
        ExecutionReport r;
        while (q_fills.pop(r)) {
            uint64_t t_now = now_ns();
            latencies[fills_received] = t_now - t_send[r.order_id];
            fills_received++;
            if (fills_received % 100000 == 0) printf("[Agent] Processed %lu fills...\n", (unsigned long)fills_received);
        }

        // 2. Try to push the next order
        auto& order = preallocated_orders[orders_sent];
        
        // Record timestamp immediately before pushing
        t_send[order->order_id] = now_ns(); 
        
        if (q_orders.push(order)) {
            orders_sent++;
        }
    }

    // Drain the remaining fills
    while (fills_received < NUM_ORDERS) {
        ExecutionReport r;
        if (q_fills.pop(r)) {
            uint64_t t_now = now_ns();
            latencies[fills_received] = t_now - t_send[r.order_id];
            fills_received++;
            if (fills_received % 100000 == 0) printf("[Agent] Processed %lu fills (drain phase)...\n", (unsigned long)fills_received);
        }
    }

    uint64_t end_time = now_ns();
    double total_time_sec = (end_time - start_time) / 1e9;
    double ops = NUM_ORDERS / total_time_sec;

    // Shutdown exchange thread
    keep_running.store(false, std::memory_order_relaxed);

    // =========================================================================
    // Stats calculation
    // =========================================================================
    std::sort(latencies.begin(), latencies.end());

    uint64_t min_lat = latencies[0];
    uint64_t max_lat = latencies[NUM_ORDERS - 1];
    uint64_t p50_lat = latencies[NUM_ORDERS * 0.50];
    uint64_t p90_lat = latencies[NUM_ORDERS * 0.90];
    uint64_t p99_lat = latencies[NUM_ORDERS * 0.99];
    uint64_t p999_lat = latencies[NUM_ORDERS * 0.999];

    uint64_t sum = 0;
    for (auto lat : latencies) sum += lat;
    uint64_t avg_lat = sum / NUM_ORDERS;

    printf("\n");
    printf(BOLD "=== BENCHMARK RESULTS ===" RESET "\n");
    printf("Total Orders:   %lu\n", (unsigned long)NUM_ORDERS);
    printf("Elapsed Time:   %.3f seconds\n", total_time_sec);
    printf(GREEN BOLD "Throughput:     %.0f orders/sec\n" RESET, ops);
    
    printf("\n");
    printf(BOLD "=== LATENCY PERCENTILES (Round-trip) ===" RESET "\n");
    printf("Min:            %lu ns\n", (unsigned long)min_lat);
    printf("Avg:            %lu ns\n", (unsigned long)avg_lat);
    printf("p50 (Median):   %lu ns\n", (unsigned long)p50_lat);
    printf("p90:            %lu ns\n", (unsigned long)p90_lat);
    printf(YELLOW "p99:            %lu ns\n" RESET, (unsigned long)p99_lat);
    printf(RED    "p99.9:          %lu ns\n" RESET, (unsigned long)p999_lat);
    printf("Max:            %lu ns\n", (unsigned long)max_lat);

    return 0;
}

// =============================================================================
// main
// =============================================================================
int main() {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("\n");
    printf(BOLD CYAN
        "+=====================================================+\n"
        "|  CPP MATCHING ENGINE -- Stage 7 Interactive Test  |\n"
        "|  High-Resolution Benchmarking Harness             |\n"
        "+=====================================================+\n"
        RESET "\n");

    printf("Pre-allocating %lu order objects to avoid heap allocation skew...\n", (unsigned long)NUM_ORDERS);
    preallocated_orders.reserve(NUM_ORDERS);
    for (size_t i = 0; i < NUM_ORDERS; i++) {
        auto o = std::make_shared<Order>();
        o->order_id = i;
        o->trader_id = 42;
        o->side = Side::BUY;
        o->type = OrderType::MARKET;
        o->price = 0.0;
        o->quantity = 1;
        o->set_symbol("AAPL");
        preallocated_orders.push_back(o);
    }
    printf("Pre-allocation complete.\n\n");

    HANDLE hExchange = CreateThread(NULL, 0, exchange_thread_func, NULL, 0, NULL);
    HANDLE hAgent    = CreateThread(NULL, 0, agent_thread_func, NULL, 0, NULL);

    WaitForSingleObject(hAgent, INFINITE);
    WaitForSingleObject(hExchange, INFINITE);

    CloseHandle(hAgent);
    CloseHandle(hExchange);

    return 0;
}
