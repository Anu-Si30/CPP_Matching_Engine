// =============================================================================
// stage9_test.cpp  —  Interactive walkthrough of Stage 9: Historical Replay
//
// In this stage, we simulate reading a raw binary NASDAQ ITCH 5.0 PCAP file.
// Since we don't have a 10GB historical file lying around, this test first
// generates a tiny `mock_itch.bin` file containing big-endian binary packets,
// and then parses it through our `ItchParser` to feed the Exchange.
//
// Build:
//   g++ -std=c++14 -g -Wall -Iinclude
//       src/core/order_book.cpp
//       src/core/matching_engine.cpp
//       src/core/exchange.cpp
//       tests/stage9_test.cpp
//       -o build/stage9_test.exe
// =============================================================================

#include "core/types.h"
#include "core/order_book.h"
#include "core/exchange.h"
#include "core/itch_parser.h"

#include <cstdio>
#include <cstring>
#include <vector>
#include <string>
#include <cmath>

#define RED    "\033[31m"
#define GREEN  "\033[32m"
#define YELLOW "\033[33m"
#define CYAN   "\033[36m"
#define BOLD   "\033[1m"
#define RESET  "\033[0m"

void divider(const char* title) {
    printf("\n" BOLD CYAN "=== %s ", title);
    int pad = 55 - (int)strlen(title);
    for (int i = 0; i < pad; i++) printf("=");
    printf(RESET "\n\n");
}

void print_book_compact(const Exchange& ex, const char* sym) {
    const OrderBook* book = ex.get_book(sym);
    if (!book) return;

    auto asks = book->ask_depth(5);
    auto bids = book->bid_depth(5);

    printf("  Book [%s]  mid=%.2f  spread=%.2f  resting=%lu\n",
           sym, book->mid_price(), book->spread(),
           (unsigned long)book->resting_order_count());

    for (int i = (int)asks.size()-1; i >= 0; i--)
        printf(RED   "    %.2f | %lu ASK\n" RESET,
               asks[i].first, (unsigned long)asks[i].second);
    for (size_t i = 0; i < bids.size(); i++)
        printf(GREEN "    %.2f | %lu BID\n" RESET,
               bids[i].first, (unsigned long)bids[i].second);
    printf("\n");
}

// Helper to write a mock ITCH Add Order message to a file
void write_mock_add_order(std::ofstream& file, uint64_t order_ref, char side, uint32_t shares, const char* symbol, uint32_t price_int) {
    uint16_t msg_length = bswap16(sizeof(ItchAddOrder) + 1); // +1 for the message type byte
    char msg_type = 'A';
    
    ItchAddOrder payload = {};
    payload.stock_locate = bswap16(1);
    payload.tracking_number = bswap16(1);
    payload.timestamp = bswap64(123456789ULL);
    payload.order_ref_num = bswap64(order_ref);
    payload.buy_sell_indicator = side;
    payload.shares = bswap32(shares);
    memset(payload.stock, ' ', 8); // pad with spaces
    memcpy(payload.stock, symbol, strlen(symbol));
    payload.price = bswap32(price_int); // 1000000 = $100.00
    
    file.write(reinterpret_cast<const char*>(&msg_length), 2);
    file.write(&msg_type, 1);
    file.write(reinterpret_cast<const char*>(&payload), sizeof(payload));
}

void generate_mock_itch_file(const char* filepath) {
    std::ofstream file(filepath, std::ios::binary);
    
    // Write a few Ask orders
    write_mock_add_order(file, 1001, 'S', 500, "AAPL", 1500500); // 150.05
    write_mock_add_order(file, 1002, 'S', 200, "AAPL", 1500200); // 150.02
    write_mock_add_order(file, 1003, 'S', 100, "AAPL", 1500100); // 150.01

    // Write a few Bid orders
    write_mock_add_order(file, 1004, 'B', 300, "AAPL", 1499500); // 149.95
    write_mock_add_order(file, 1005, 'B', 150, "AAPL", 1499800); // 149.98
    write_mock_add_order(file, 1006, 'B', 800, "AAPL", 1499900); // 149.99
    
    // Write a crossing order (should execute against the asks)
    write_mock_add_order(file, 1007, 'B', 250, "AAPL", 1500300); // Buy 250 @ 150.03
    
    file.close();
}

void test_historical_replay() {
    divider("TEST 1: Parse and Replay NASDAQ ITCH 5.0");

    const char* filepath = "mock_itch.bin";
    
    printf("1. Generating mock binary ITCH 5.0 file: %s\n", filepath);
    generate_mock_itch_file(filepath);
    
    printf("2. Parsing ITCH messages through byte-swapping logic...\n\n");
    auto parsed_orders = ItchParser::parse_file(filepath);
    
    printf("Successfully parsed %lu messages.\n", (unsigned long)parsed_orders.size());
    for (auto& o : parsed_orders) {
        printf("  Parsed -> %s %u shares of %s @ $%.2f (ID: %lu)\n", 
               o->side == Side::BUY ? GREEN "BUY " RESET : RED "SELL" RESET,
               o->quantity, o->symbol, o->price, (unsigned long)o->order_id);
    }
    printf("\n");

    Exchange ex([](const ExecutionReport& r) {
        printf(YELLOW "  => ExecutionReport: Traded %u shares of %s @ $%.2f\n" RESET, 
               r.exec_qty, r.symbol, r.exec_price);
    });
    ex.add_symbol("AAPL");

    printf("3. Replaying historical messages into the Exchange...\n");
    for (auto& o : parsed_orders) {
        ex.submit_order(o);
    }
    printf("\n");

    printf("Final Order Book State:\n");
    print_book_compact(ex, "AAPL");
    
    // The crossing order (Buy 250 @ 150.03) should have taken out the 100 shares at 150.01,
    // and 150 shares at 150.02.
    const OrderBook* book = ex.get_book("AAPL");
    bool pass = (std::abs(book->mid_price() - 150.005) < 1e-5) && 
                (std::abs(book->spread() - 0.03) < 1e-5);
    
    printf(pass ? GREEN BOLD "PASS — Historical messages successfully matched\n" RESET 
                : RED BOLD "FAIL — Final book state incorrect\n" RESET);
}

int main() {
    printf("\n");
    printf(BOLD CYAN
        "+=====================================================+\n"
        "|  CPP MATCHING ENGINE -- Stage 9 Interactive Test  |\n"
        "|  NASDAQ ITCH 5.0 Binary Protocol Replay           |\n"
        "+=====================================================+\n"
        RESET "\n");

    test_historical_replay();

    divider("SUMMARY");
    printf("Stage 9 completes the pipeline:\n");
    printf("  Binary Protocol Parsing — `#pragma pack(push, 1)` memory mapping\n");
    printf("  Endianness Byte-Swapping — Network Big-Endian to Host Little-Endian\n");
    printf("  Historical Data Backtesting — Piping raw payloads into the engine\n\n");
    
    return 0;
}
