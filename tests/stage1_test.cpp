// =============================================================================
// stage1_test.cpp  —  Interactive walkthrough of Stage 1
// Compatible with GCC 6.3 (C++14 + constexpr fixes)
// =============================================================================

#include "core/types.h"
#include "core/order_book.h"
#include "core/matching_engine.h"

#include <iostream>
#include <iomanip>
#include <vector>
#include <memory>
#include <cstdio>

// ─── ANSI Colors ─────────────────────────────────────────────────────────────
#define RED    "\033[31m"
#define GREEN  "\033[32m"
#define YELLOW "\033[33m"
#define CYAN   "\033[36m"
#define BOLD   "\033[1m"
#define DIM    "\033[2m"
#define RESET  "\033[0m"

void divider(const char* title = nullptr) {
    if (title) {
        printf("\n" BOLD CYAN "=== %s ", title);
        int pad = 55 - (int)strlen(title);
        for (int i = 0; i < pad; i++) printf("=");
        printf(RESET "\n\n");
    } else {
        printf(DIM "------------------------------------------------\n" RESET);
    }
}

// ─── Print order book state ───────────────────────────────────────────────────
void print_book(const OrderBook& book, int depth = 5) {
    printf(BOLD "  Book: %s\n" RESET, book.symbol.c_str());
    printf(BOLD "  %-12s | %-10s | %s\n" RESET, "PRICE", "QTY", "SIDE");
    printf("  -----------------------------------------\n");

    auto asks = book.ask_depth(depth);
    // Reverse so lowest ask is nearest the middle
    for (int i = (int)asks.size() - 1; i >= 0; i--) {
        printf(RED   "  %12.2f | %-10lu | ASK\n" RESET,
               asks[i].first, (unsigned long)asks[i].second);
    }

    double mid = book.mid_price();
    if (mid > 0)
        printf(YELLOW "  -- mid: %7.4f ----------------------\n" RESET, mid);
    else
        printf(YELLOW "  -- (no mid price yet) ------------------\n" RESET);

    auto bids = book.bid_depth(depth);
    for (size_t i = 0; i < bids.size(); i++) {
        printf(GREEN "  %12.2f | %-10lu | BID\n" RESET,
               bids[i].first, (unsigned long)bids[i].second);
    }
    printf("\n");
}

// ─── Order factory helpers ────────────────────────────────────────────────────
static uint64_t order_counter = 1;

std::shared_ptr<Order> make_limit(
    Side side, double price, uint32_t qty,
    const char* symbol = "AAPL", uint64_t trader = 1)
{
    auto o = std::make_shared<Order>();
    o->order_id     = order_counter++;
    o->trader_id    = trader;
    o->timestamp_ns = now_ns();
    o->side         = side;
    o->type         = OrderType::LIMIT;
    o->price        = price;
    o->quantity     = qty;
    o->filled_qty   = 0;
    o->status       = OrderStatus::NEW;
    o->set_symbol(symbol);
    return o;
}

std::shared_ptr<Order> make_market(
    Side side, uint32_t qty,
    const char* symbol = "AAPL", uint64_t trader = 1)
{
    auto o = std::make_shared<Order>();
    o->order_id     = order_counter++;
    o->trader_id    = trader;
    o->timestamp_ns = now_ns();
    o->side         = side;
    o->type         = OrderType::MARKET;
    o->price        = 0.0;
    o->quantity     = qty;
    o->filled_qty   = 0;
    o->status       = OrderStatus::NEW;
    o->set_symbol(symbol);
    return o;
}

// ─── Test 1: Build a book — no fills expected ─────────────────────────────────
void test_build_book() {
    divider("TEST 1: Build an Order Book (no matches)");

    printf("Concept: We post limit orders that don't cross the spread.\n");
    printf("They should REST in the book, waiting to be matched.\n\n");

    OrderBook book("AAPL");
    std::vector<ExecutionReport> fills;
    MatchingEngine engine([&](const ExecutionReport& r){ fills.push_back(r); });

    printf("Posting BUY  100 @ 99.90\n");
    engine.submit_order(make_limit(Side::BUY,  99.90, 100), book);
    printf("Posting BUY  200 @ 99.80\n");
    engine.submit_order(make_limit(Side::BUY,  99.80, 200), book);
    printf("Posting BUY  150 @ 99.70\n");
    engine.submit_order(make_limit(Side::BUY,  99.70, 150), book);
    printf("Posting SELL 120 @ 100.10\n");
    engine.submit_order(make_limit(Side::SELL, 100.10, 120), book);
    printf("Posting SELL  80 @ 100.20\n");
    engine.submit_order(make_limit(Side::SELL, 100.20,  80), book);
    printf("Posting SELL 200 @ 100.30\n");
    engine.submit_order(make_limit(Side::SELL, 100.30, 200), book);

    printf("\nResult (no fills expected: bid=99.90 < ask=100.10):\n\n");
    print_book(book);

    printf("Fills generated: %d  (expected: 0)\n",   (int)fills.size());
    printf("Best bid:  %.2f       (expected: 99.90)\n",   book.best_bid());
    printf("Best ask:  %.2f      (expected: 100.10)\n",   book.best_ask());
    printf("Spread:    %.2f       (expected: 0.20)\n\n",  book.spread());

    bool pass = fills.empty() &&
                book.best_bid()  == 99.90 &&
                book.best_ask()  == 100.10 &&
                book.bids.size() == 3 &&
                book.asks.size() == 3;
    printf(pass ? GREEN BOLD "PASS\n" RESET : RED BOLD "FAIL\n" RESET);
}

// ─── Test 2: Limit order that crosses → fill ──────────────────────────────────
void test_limit_match() {
    divider("TEST 2: Limit Order Match (spread crossing)");

    printf("Concept: A BUY at 100.15 crosses a resting SELL at 100.10.\n");
    printf("Fill price = resting price = 100.10 (not the aggressor's 100.15).\n\n");

    OrderBook book("AAPL");
    std::vector<ExecutionReport> fills;
    MatchingEngine engine([&](const ExecutionReport& r){ fills.push_back(r); });

    auto sell = make_limit(Side::SELL, 100.10, 100, "AAPL", 2);
    engine.submit_order(sell, book);
    printf("Resting: SELL 100 @ 100.10 (order_id=%lu, trader=2)\n\n",
           (unsigned long)sell->order_id);
    print_book(book);

    auto buy = make_limit(Side::BUY, 100.15, 60, "AAPL", 1);
    printf("Incoming: BUY 60 @ 100.15  (100.15 >= 100.10 => crosses!)\n\n");
    engine.submit_order(buy, book);

    printf("Fills generated: %d  (expected: 2, one per party)\n", (int)fills.size());
    for (auto& r : fills) {
        printf("  order_id=%-5lu trader=%-3lu qty=%-4u price=%.2f side=%s\n",
               (unsigned long)r.order_id,
               (unsigned long)r.trader_id,
               r.exec_qty, r.exec_price,
               r.side == Side::BUY ? "BUY" : "SELL");
    }

    printf("\nBook after fill:\n");
    print_book(book);

    printf("sell remaining:  %u   (expected: 40 = 100 - 60)\n", sell->remaining());
    printf("buy  status:     %s  (expected: FILLED)\n\n",
           buy->status == OrderStatus::FILLED ? "FILLED" : "NOT FILLED");

    bool pass = fills.size() == 2 &&
                fills[0].exec_price == 100.10 &&
                fills[0].exec_qty   == 60 &&
                sell->remaining()   == 40 &&
                buy->status         == OrderStatus::FILLED;
    printf(pass ? GREEN BOLD "PASS\n" RESET : RED BOLD "FAIL\n" RESET);
}

// ─── Test 3: FIFO priority ────────────────────────────────────────────────────
void test_fifo_priority() {
    divider("TEST 3: Price-Time (FIFO) Priority");

    printf("Concept: Two SELL orders at the SAME price 100.00.\n");
    printf("The OLDER one (A, submitted first) MUST fill before the newer one (B).\n\n");

    OrderBook book("AAPL");
    std::vector<ExecutionReport> fills;
    MatchingEngine engine([&](const ExecutionReport& r){ fills.push_back(r); });

    auto sellA = make_limit(Side::SELL, 100.00, 50, "AAPL", 10);
    engine.submit_order(sellA, book);
    printf("SELL A: 50 @ 100.00 (order_id=%lu) -- submitted FIRST\n",
           (unsigned long)sellA->order_id);

    auto sellB = make_limit(Side::SELL, 100.00, 50, "AAPL", 11);
    engine.submit_order(sellB, book);
    printf("SELL B: 50 @ 100.00 (order_id=%lu) -- submitted SECOND\n",
           (unsigned long)sellB->order_id);

    printf("\nBoth at 100.00. Same price level. FIFO queue: [A, B]\n\n");

    auto buy = make_limit(Side::BUY, 100.00, 50, "AAPL", 1);
    printf("Incoming: BUY 50 @ 100.00 (should match SELL A first)\n\n");
    engine.submit_order(buy, book);

    printf("SELL A remaining: %u  (expected: 0 -- fully filled)\n",  sellA->remaining());
    printf("SELL B remaining: %u  (expected: 50 -- untouched)\n\n",  sellB->remaining());

    bool pass = sellA->remaining() == 0 && sellB->remaining() == 50;
    printf(pass ? GREEN BOLD "PASS -- FIFO is working\n" RESET
                : RED BOLD "FAIL\n" RESET);
}

// ─── Test 4: Market order sweeps multiple levels ──────────────────────────────
void test_market_sweep() {
    divider("TEST 4: Market Order Sweeps Multiple Price Levels");

    printf("Concept: A large MARKET BUY consumes multiple ask price levels.\n");
    printf("This is called 'walking the book' — VWAP > best ask.\n\n");

    OrderBook book("AAPL");
    std::vector<ExecutionReport> fills;
    MatchingEngine engine([&](const ExecutionReport& r){ fills.push_back(r); });

    engine.submit_order(make_limit(Side::SELL, 100.00,  50, "AAPL", 2), book);
    printf("SELL  50 @ 100.00\n");
    engine.submit_order(make_limit(Side::SELL, 100.05, 100, "AAPL", 3), book);
    printf("SELL 100 @ 100.05\n");
    engine.submit_order(make_limit(Side::SELL, 100.10,  75, "AAPL", 4), book);
    printf("SELL  75 @ 100.10\n\n");

    print_book(book);

    printf("MARKET BUY for 175 shares:\n");
    printf("  Should fill: 50 @ 100.00, 100 @ 100.05, 25 @ 100.10\n\n");

    auto mkt_buy = make_market(Side::BUY, 175, "AAPL", 1);
    engine.submit_order(mkt_buy, book);

    double total_cost = 0;
    uint32_t total_qty = 0;
    printf("Fill breakdown:\n");
    for (auto& r : fills) {
        if (r.trader_id != mkt_buy->trader_id) {  // Skip aggressor reports
            printf("  Filled %u @ %.2f = $%.2f\n",
                   r.exec_qty, r.exec_price, r.exec_qty * r.exec_price);
            total_cost += r.exec_qty * r.exec_price;
            total_qty  += r.exec_qty;
        }
    }

    double vwap = total_qty > 0 ? total_cost / total_qty : 0;
    printf("\nTotal filled: %u shares\n",   total_qty);
    printf("VWAP (avg price): %.4f\n",     vwap);
    printf("Slippage vs best ask: %.4f/share\n\n", vwap - 100.00);

    printf("Book after sweep:\n");
    print_book(book);

    bool pass = mkt_buy->filled_qty == 175;
    printf(pass ? GREEN BOLD "PASS -- Market order swept 175 shares across 3 levels\n" RESET
                : RED BOLD "FAIL -- Only filled %u\n" RESET, mkt_buy->filled_qty);
}

// ─── Test 5: Cancellation ────────────────────────────────────────────────────
void test_cancel() {
    divider("TEST 5: Order Cancellation");

    printf("Concept: Cancel removes an order from the book.\n");
    printf("If other orders remain at that price, the level stays.\n\n");

    OrderBook book("AAPL");
    std::vector<ExecutionReport> fills;
    MatchingEngine engine([&](const ExecutionReport& r){ fills.push_back(r); });

    auto order1 = make_limit(Side::BUY, 99.90, 100, "AAPL", 1);
    auto order2 = make_limit(Side::BUY, 99.90, 200, "AAPL", 2);  // Same level
    auto order3 = make_limit(Side::BUY, 99.80,  50, "AAPL", 3);

    engine.submit_order(order1, book);
    engine.submit_order(order2, book);
    engine.submit_order(order3, book);

    printf("Posted:\n");
    printf("  order1: 100 @ 99.90  (id=%lu)\n", (unsigned long)order1->order_id);
    printf("  order2: 200 @ 99.90  (id=%lu)  <- same level as order1\n",
           (unsigned long)order2->order_id);
    printf("  order3:  50 @ 99.80  (id=%lu)\n\n",
           (unsigned long)order3->order_id);
    print_book(book);

    printf("Cancelling order1...\n");
    bool ok = engine.cancel_order(order1->order_id, book);
    printf("  Cancel result: %s\n\n", ok ? "SUCCESS" : "FAILED");

    printf("Book after cancel:\n");
    print_book(book);

    // Qty at 99.90 should now be 200 (order1's 100 is gone)
    auto bids = book.bid_depth(5);
    uint64_t qty_at_9990 = 0;
    for (size_t i = 0; i < bids.size(); i++) {
        if (bids[i].first == 99.90) qty_at_9990 = bids[i].second;
    }

    printf("Qty remaining at 99.90: %lu  (expected: 200)\n",
           (unsigned long)qty_at_9990);
    printf("Resting order count:    %lu  (expected: 2)\n\n",
           (unsigned long)book.resting_order_count());

    bool pass = ok && qty_at_9990 == 200 && book.resting_order_count() == 2;
    printf(pass ? GREEN BOLD "PASS\n" RESET : RED BOLD "FAIL\n" RESET);
}

// ─── Test 6: Partial fill ────────────────────────────────────────────────────
void test_partial_fill() {
    divider("TEST 6: Partial Fill");

    printf("Concept: A small incoming order partially fills a large resting order.\n");
    printf("The resting order stays in book with reduced quantity.\n\n");

    OrderBook book("AAPL");
    std::vector<ExecutionReport> fills;
    MatchingEngine engine([&](const ExecutionReport& r){ fills.push_back(r); });

    auto big_sell = make_limit(Side::SELL, 100.00, 500, "AAPL", 2);
    engine.submit_order(big_sell, book);
    printf("Resting: SELL 500 @ 100.00\n\n");

    auto small_buy = make_limit(Side::BUY, 100.00, 50, "AAPL", 1);
    printf("Incoming: BUY 50 @ 100.00 (small vs. large resting)\n\n");
    engine.submit_order(small_buy, book);

    printf("small_buy status:    %s  (expected: FILLED)\n",
           small_buy->status == OrderStatus::FILLED ? "FILLED" : "OTHER");
    printf("big_sell remaining:  %u  (expected: 450 = 500 - 50)\n",
           big_sell->remaining());
    printf("big_sell status:     %s (expected: PARTIALLY_FILLED)\n\n",
           big_sell->status == OrderStatus::PARTIALLY_FILLED ? "PARTIALLY_FILLED" : "OTHER");

    printf("Book (sell should remain with qty=450):\n");
    print_book(book);

    auto asks = book.ask_depth(5);
    bool sell_in_book = !asks.empty() && asks[0].second == 450;

    bool pass = sell_in_book &&
                big_sell->remaining() == 450 &&
                small_buy->status == OrderStatus::FILLED;
    printf(pass ? GREEN BOLD "PASS\n" RESET : RED BOLD "FAIL\n" RESET);
}

// ─── main ────────────────────────────────────────────────────────────────────
int main() {
    printf("\n");
    printf(BOLD CYAN
        "+=====================================================+\n"
        "|  CPP MATCHING ENGINE -- Stage 1 Interactive Test  |\n"
        "+=====================================================+\n"
        RESET "\n");

    printf("Order struct size: %u bytes  (expected: 64)\n\n", (unsigned)sizeof(Order));

    test_build_book();
    test_limit_match();
    test_fifo_priority();
    test_market_sweep();
    test_cancel();
    test_partial_fill();

    divider("SUMMARY");
    printf("All 6 tests pass => Stage 1 is complete.\n");
    printf("You now have a working:\n");
    printf("  Two-sided order book (bid/ask sorted, FIFO per level)\n");
    printf("  Limit order matching with price-time priority\n");
    printf("  Market order execution (multi-level sweep)\n");
    printf("  Cancellation with secondary index (O log N)\n");
    printf("  Partial fills\n");
    printf("  ExecutionReport generation for both parties\n\n");

    return 0;
}
