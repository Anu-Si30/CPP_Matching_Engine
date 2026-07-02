// =============================================================================
// mercury_server.cpp  —  The main entry point for the standalone server
//
// To compile:
// g++ -std=c++14 -O3 -Wall -Isrc src/orderbook/order_book.cpp src/matching/matching_engine.cpp src/exchange/exchange.cpp src/networking/mercury_server.cpp -o build/mercury_server.exe -lws2_32
// =============================================================================

#include "exchange/exchange.h"
#include "networking/tcp_server.h"
#include "traders/market_maker.h"
#include "traders/random_trader.h"

#include <iostream>
#include <windows.h>

int main() {
    std::cout << "Starting MERCURY Exchange Server...\n";

    Exchange ex([](const ExecutionReport& r) {
        // Drop standard fills to /dev/null for performance, or log large ones
    });

    ex.add_symbol("AAPL");

    // Start a Market Maker to provide baseline liquidity
    MarketMaker mm(1, &ex);
    mm.set_symbol("AAPL");
    
    RandomTrader noise(2);
    noise.set_symbol("AAPL");

    // The lock-free queue that accepts external orders
    SPSCQueue<std::shared_ptr<Order>> external_queue(1024);

    TcpServer server(8080, &external_queue);
    server.start();

    std::cout << "Exchange Engine running on Thread " << GetCurrentThreadId() << "\n";
    std::cout << "Awaiting external TCP connections on port 8080...\n";
    std::cout << "Press Ctrl+C to exit.\n";

    uint64_t tick = 0;
    while (true) {
        // 1. Process internal agents
        auto mm_orders = mm.on_tick(*ex.get_book("AAPL"), tick);
        for (auto& o : mm_orders) ex.submit_order(o);

        auto noise_orders = noise.on_tick(*ex.get_book("AAPL"), tick);
        for (auto& o : noise_orders) ex.submit_order(o);

        // 2. Poll the external queue for network orders
        std::shared_ptr<Order> ext_order;
        while (external_queue.pop(ext_order)) {
            std::cout << "[Exchange] Received external order: " 
                      << (ext_order->side == Side::BUY ? "BUY " : "SELL ")
                      << ext_order->quantity << " @ $" << ext_order->price << "\n";
            ex.submit_order(ext_order);
        }

        Sleep(10);
        tick++;
    }

    server.stop();
    return 0;
}
