// =============================================================================
// stage2_test.cpp  —  Interactive walkthrough of Stage 2
//
// Stage 2 adds three things on top of Stage 1:
//   1. Order Modification  (change price or qty of a resting order)
//   2. Multiple Symbols    (AAPL, MSFT, TSLA all active simultaneously)
//   3. Exchange class      (single entry point that routes everything)
//
// Build:
//   g++ -std=c++14 -g -Wall -Isrc
//       src/orderbook/order_book.cpp
//       src/matching/matching_engine.cpp
//       src/exchange/exchange.cpp
//       tests/stage2_test.cpp
//       -o build/stage2_test.exe
//
// Run: ./build/stage2_test.exe
// =============================================================================

#include "orderbook/types.h"
#include "orderbook/order_book.h"
#include "matching/matching_engine.h"
#include "exchange/exchange.h"

#include <cstdio>
#include <cstring>
#include <vector>
#include <memory>
#include <algorithm>

// ─── ANSI ─────────────────────────────────────────────────────────────────────
#define RED    "\033[31m"
#define GREEN  "\033[32m"
#define YELLOW "\033[33m"
#define CYAN   "\033[36m"
#define BOLD   "\033[1m"
#define DIM    "\033[2m"
#define RESET  "\033[0m"

void divider(const char* title) {
    printf("\n" BOLD CYAN "=== %s ", title);
    int pad = 55 - (int)strlen(title);
    for (int i = 0; i < pad; i++) printf("=");
    printf(RESET "\n\n");
}

// ─── Print a single book ──────────────────────────────────────────────────────
void print_book(const Exchange& ex, const char* symbol, int depth = 5) {
    const OrderBook* book = ex.get_book(symbol);
    if (!book) {
        printf("  [Book for %s not found]\n", symbol);
        return;
    }
    printf(BOLD "  %s Order Book\n" RESET, symbol);
    printf("  %-12s | %-10s | %s\n", "PRICE", "QTY", "SIDE");
    printf("  -----------------------------------------\n");

    auto asks = book->ask_depth(depth);
    for (int i = (int)asks.size() - 1; i >= 0; i--)
        printf(RED "  %12.2f | %-10lu | ASK\n" RESET,
               asks[i].first, (unsigned long)asks[i].second);

    double mid = book->mid_price();
    if (mid > 0)
        printf(YELLOW "  -- mid: %7.4f ---------------------------\n" RESET, mid);
    else
        printf(YELLOW "  -- (no mid yet) ----------------------------\n" RESET);

    auto bids = book->bid_depth(depth);
    for (size_t i = 0; i < bids.size(); i++)
        printf(GREEN "  %12.2f | %-10lu | BID\n" RESET,
               bids[i].first, (unsigned long)bids[i].second);
    printf("\n");
}

// ─── Order factory ────────────────────────────────────────────────────────────
static uint64_t oid = 1;

std::shared_ptr<Order> make_limit(Side side, double price, uint32_t qty,
                                  const char* sym, uint64_t trader = 1) {
    auto o          = global_order_pool.acquire();
    o->order_id     = oid++;
    o->trader_id    = trader;
    o->timestamp_ns = now_ns();
    o->side         = side;
    o->type         = OrderType::LIMIT;
    o->price        = price;
    o->quantity     = qty;
    o->filled_qty   = 0;
    o->status       = OrderStatus::NEW;
    o->set_symbol(sym);
    return o;
}

std::shared_ptr<Order> make_market(Side side, uint32_t qty, const char* sym,
                                   uint64_t trader = 1) {
    auto o          = global_order_pool.acquire();
    o->order_id     = oid++;
    o->trader_id    = trader;
    o->timestamp_ns = now_ns();
    o->side         = side;
    o->type         = OrderType::MARKET;
    o->price        = 0.0;
    o->quantity     = qty;
    o->filled_qty   = 0;
    o->status       = OrderStatus::NEW;
    o->set_symbol(sym);
    return o;
}

// =============================================================================
// TEST 1 — Multiple symbols on one Exchange
// =============================================================================
void test_multiple_symbols() {
    divider("TEST 1: Multiple Symbols on One Exchange");

    printf("Concept: The Exchange routes orders to per-symbol books.\n");
    printf("AAPL orders go to AAPL's book. MSFT orders to MSFT's book.\n");
    printf("They are completely independent — fills in AAPL don't affect MSFT.\n\n");

    std::vector<ExecutionReport> fills;
    Exchange ex([&](const ExecutionReport& r){ fills.push_back(r); });

    // List three symbols
    ex.add_symbol("AAPL");
    ex.add_symbol("MSFT");
    ex.add_symbol("TSLA");

    printf("Listed symbols: ");
    for (auto& s : ex.list_symbols()) printf("%s  ", s.c_str());
    printf("\n\n");

    // ── Build AAPL book ──────────────────────────────────────────────────────
    printf("Building AAPL book:\n");
    ex.submit_order(make_limit(Side::BUY,  149.90, 200, "AAPL", 1));
    printf("  BUY  200 @ 149.90 (AAPL)\n");
    ex.submit_order(make_limit(Side::SELL, 150.10, 150, "AAPL", 2));
    printf("  SELL 150 @ 150.10 (AAPL)\n");

    // ── Build MSFT book ──────────────────────────────────────────────────────
    printf("\nBuilding MSFT book:\n");
    ex.submit_order(make_limit(Side::BUY,  330.00, 100, "MSFT", 3));
    printf("  BUY  100 @ 330.00 (MSFT)\n");
    ex.submit_order(make_limit(Side::SELL, 330.50, 100, "MSFT", 4));
    printf("  SELL 100 @ 330.50 (MSFT)\n");

    // ── Build TSLA book ──────────────────────────────────────────────────────
    printf("\nBuilding TSLA book:\n");
    ex.submit_order(make_limit(Side::BUY,  240.00, 300, "TSLA", 5));
    printf("  BUY  300 @ 240.00 (TSLA)\n");
    ex.submit_order(make_limit(Side::SELL, 241.00, 300, "TSLA", 6));
    printf("  SELL 300 @ 241.00 (TSLA)\n");

    printf("\nNo fills yet (no crosses).\n");
    printf("Fills so far: %d  (expected: 0)\n\n", (int)fills.size());

    print_book(ex, "AAPL");
    print_book(ex, "MSFT");
    print_book(ex, "TSLA");

    // ── Cross AAPL only ──────────────────────────────────────────────────────
    printf("Crossing AAPL: MARKET BUY 50 @ AAPL\n\n");
    int fills_before = (int)fills.size();
    ex.submit_order(make_market(Side::BUY, 50, "AAPL", 99));
    int new_fills = (int)fills.size() - fills_before;

    printf("New fills: %d  (expected: 2 — one per party)\n", new_fills);
    printf("AAPL best ask qty now: ");
    const OrderBook* aapl = ex.get_book("AAPL");
    auto asks = aapl->ask_depth(1);
    printf("%lu  (expected: 100 = 150-50)\n", asks.empty() ? 0 : (unsigned long)asks[0].second);
    printf("MSFT and TSLA unchanged (isolated books).\n\n");

    // ── Reject unknown symbol ────────────────────────────────────────────────
    printf("Attempting order for unlisted symbol 'NVDA':\n");
    auto bad = make_limit(Side::BUY, 500.00, 10, "NVDA", 1);
    bool ok = ex.submit_order(bad);
    printf("  submit_order returned: %s  (expected: false)\n", ok ? "true" : "false");
    printf("  Order status: %s  (expected: REJECTED)\n\n",
           bad->status == OrderStatus::REJECTED ? "REJECTED" : "OTHER");

    bool pass = fills.size() == 2 &&
                !asks.empty() && asks[0].second == 100 &&
                !ok &&
                bad->status == OrderStatus::REJECTED;
    printf(pass ? GREEN BOLD "PASS\n" RESET : RED BOLD "FAIL\n" RESET);
}

// =============================================================================
// TEST 2 — Order Modification: price change (cancel + reinsert)
// =============================================================================
void test_modify_price() {
    divider("TEST 2: Modify Price (Cancel + Reinsert, Queue Position Lost)");

    printf("Concept: Changing price = new order economically.\n");
    printf("The order goes to the BACK of the new price level.\n");
    printf("This prevents gaming the queue: you can't hold a front-of-queue slot\n");
    printf("at a price you never intend to trade at.\n\n");

    std::vector<ExecutionReport> fills;
    Exchange ex([&](const ExecutionReport& r){ fills.push_back(r); });
    ex.add_symbol("AAPL");

    // Two sells at 100.00 — A was first, B was second
    auto sellA = make_limit(Side::SELL, 100.00, 50, "AAPL", 10);
    auto sellB = make_limit(Side::SELL, 100.00, 50, "AAPL", 11);
    ex.submit_order(sellA);
    ex.submit_order(sellB);

    printf("Initial queue at $100.00: [SELL A (id=%lu), SELL B (id=%lu)]\n",
           (unsigned long)sellA->order_id, (unsigned long)sellB->order_id);
    printf("FIFO: A was first, so A would fill before B.\n\n");

    // Now modify A's price → it gets a new timestamp, goes to back of any queue
    printf("Modifying SELL A: price 100.00 -> 99.90 (better for buyers)\n");
    bool ok = ex.modify_order(sellA->order_id, 99.90, 50);
    printf("  modify_order returned: %s\n\n", ok ? "true" : "false");

    print_book(ex, "AAPL");

    // Now a buy at 99.90 should fill A (it moved there)
    // And a buy at 100.00 should fill B (it's now alone at 100.00)
    printf("BUY 50 @ 99.90: should fill the modified SELL A\n");
    auto buy1 = make_limit(Side::BUY, 99.90, 50, "AAPL", 1);
    int before = (int)fills.size();
    ex.submit_order(buy1);
    int got = (int)fills.size() - before;
    printf("  Fills: %d  sellA filled_qty: %u  (expected: 50)\n\n",
           got, sellA->filled_qty);

    printf("BUY 50 @ 100.00: should fill SELL B (still at 100.00)\n");
    auto buy2 = make_limit(Side::BUY, 100.00, 50, "AAPL", 1);
    before = (int)fills.size();
    ex.submit_order(buy2);
    got = (int)fills.size() - before;
    printf("  Fills: %d  sellB filled_qty: %u  (expected: 50)\n\n",
           got, sellB->filled_qty);

    bool pass = ok &&
                sellA->filled_qty == 50 &&
                sellB->filled_qty == 50;
    printf(pass ? GREEN BOLD "PASS\n" RESET : RED BOLD "FAIL\n" RESET);
}

// =============================================================================
// TEST 3 — Order Modification: quantity decrease (in-place, keep position)
// =============================================================================
void test_modify_qty_decrease() {
    divider("TEST 3: Modify Qty Down (In-Place, Queue Position Kept)");

    printf("Concept: Reducing quantity is a sign of good faith — you're reducing risk.\n");
    printf("Most exchanges let you do this WITHOUT losing your queue position.\n");
    printf("This is the one modification that doesn't punish the trader.\n\n");

    std::vector<ExecutionReport> fills;
    Exchange ex([&](const ExecutionReport& r){ fills.push_back(r); });
    ex.add_symbol("AAPL");

    auto sellA = make_limit(Side::SELL, 100.00, 200, "AAPL", 10);
    auto sellB = make_limit(Side::SELL, 100.00,  50, "AAPL", 11);
    ex.submit_order(sellA);
    ex.submit_order(sellB);

    printf("Queue at $100.00: [SELL A: 200 shares, SELL B: 50 shares]\n");
    printf("Total qty at level: 250\n\n");

    printf("Reducing SELL A from 200 -> 80 (in-place, keeps front position)\n");
    bool ok = ex.modify_order(sellA->order_id, 100.00, 80);
    printf("  modify_order: %s\n", ok ? "ok" : "FAILED");

    const OrderBook* book = ex.get_book("AAPL");
    auto asks = book->ask_depth(1);
    uint64_t total = asks.empty() ? 0 : asks[0].second;
    printf("  Level total qty: %lu  (expected: 130 = 80+50)\n\n", (unsigned long)total);

    // Buy 80 — should hit SELL A (it's still at front despite modification)
    printf("BUY 80: should hit SELL A first (still at front of queue)\n");
    auto buy = make_limit(Side::BUY, 100.00, 80, "AAPL", 1);
    int before = (int)fills.size();
    ex.submit_order(buy);
    int got = (int)fills.size() - before;
    printf("  Fills: %d  sellA filled: %u  sellB remaining: %u\n\n",
           got, sellA->filled_qty, sellB->remaining());

    bool pass = ok && total == 130 && sellA->filled_qty == 80 && sellB->remaining() == 50;
    printf(pass ? GREEN BOLD "PASS\n" RESET : RED BOLD "FAIL\n" RESET);
}

// =============================================================================
// TEST 4 — Order Modification: quantity increase (cancel + reinsert)
// =============================================================================
void test_modify_qty_increase() {
    divider("TEST 4: Modify Qty Up (Cancel + Reinsert, Queue Position Lost)");

    printf("Concept: Increasing quantity = you want more — get back in line.\n");
    printf("Same treatment as a price change: cancel + reinsert with new timestamp.\n\n");

    std::vector<ExecutionReport> fills;
    Exchange ex([&](const ExecutionReport& r){ fills.push_back(r); });
    ex.add_symbol("AAPL");

    auto sellA = make_limit(Side::SELL, 100.00,  50, "AAPL", 10);
    auto sellB = make_limit(Side::SELL, 100.00, 100, "AAPL", 11);
    ex.submit_order(sellA);
    ex.submit_order(sellB);

    printf("Queue: [SELL A: 50, SELL B: 100]\n");
    printf("Without modification, A fills first.\n\n");

    printf("Increasing SELL A from 50 -> 150 (loses position, goes to back)\n");
    ex.modify_order(sellA->order_id, 100.00, 150);
    printf("Queue should now be: [SELL B: 100, SELL A: 150]\n\n");

    // A buy of 100 should now hit B (A went to the back)
    printf("BUY 100: should hit SELL B (now at front after A was reinserted)\n");
    auto buy = make_limit(Side::BUY, 100.00, 100, "AAPL", 1);
    int before = (int)fills.size();
    ex.submit_order(buy);
    int got = (int)fills.size() - before;
    printf("  Fills: %d  sellB filled: %u  (expected: 100)\n",
           got, sellB->filled_qty);
    printf("  sellA remaining: %u  (expected: 150 — untouched)\n\n",
           sellA->remaining());

    bool pass = sellB->filled_qty == 100 && sellA->remaining() == 150;
    printf(pass ? GREEN BOLD "PASS\n" RESET : RED BOLD "FAIL\n" RESET);
}

// =============================================================================
// TEST 5 — Cross-symbol independence
// =============================================================================
void test_cross_symbol_independence() {
    divider("TEST 5: Cross-Symbol Independence + Global Cancel");

    printf("Concept: Orders in one symbol's book never affect another.\n");
    printf("Also: cancel_order works by order_id alone — no symbol needed.\n\n");

    std::vector<ExecutionReport> fills;
    Exchange ex([&](const ExecutionReport& r){ fills.push_back(r); });
    ex.add_symbol("AAPL");
    ex.add_symbol("MSFT");

    auto aapl_sell = make_limit(Side::SELL, 150.00, 100, "AAPL", 1);
    auto msft_sell = make_limit(Side::SELL, 330.00, 100, "MSFT", 2);
    auto aapl_buy  = make_limit(Side::BUY,  149.50, 100, "AAPL", 3);
    auto msft_buy  = make_limit(Side::BUY,  329.50, 100, "MSFT", 4);

    ex.submit_order(aapl_sell);
    ex.submit_order(msft_sell);
    ex.submit_order(aapl_buy);
    ex.submit_order(msft_buy);

    printf("Posted: AAPL sell/buy, MSFT sell/buy — no crosses yet.\n");
    printf("Resting orders: AAPL book=%lu, MSFT book=%lu\n\n",
           (unsigned long)ex.get_book("AAPL")->resting_order_count(),
           (unsigned long)ex.get_book("MSFT")->resting_order_count());

    // Cancel the MSFT sell by ID only — no symbol argument needed
    printf("cancel_order(%lu) — MSFT sell, no symbol arg needed:\n",
           (unsigned long)msft_sell->order_id);
    bool ok = ex.cancel_order(msft_sell->order_id);
    printf("  Result: %s\n", ok ? "cancelled" : "FAILED");
    printf("  MSFT book resting: %lu  (expected: 1 — only msft_buy remains)\n",
           (unsigned long)ex.get_book("MSFT")->resting_order_count());
    printf("  AAPL book resting: %lu  (expected: 2 — untouched)\n\n",
           (unsigned long)ex.get_book("AAPL")->resting_order_count());

    // Double-cancel should fail gracefully
    printf("cancel_order(%lu) again (already cancelled):\n",
           (unsigned long)msft_sell->order_id);
    bool ok2 = ex.cancel_order(msft_sell->order_id);
    printf("  Result: %s  (expected: false)\n\n", ok2 ? "true" : "false");

    bool pass = ok && !ok2 &&
                ex.get_book("MSFT")->resting_order_count() == 1 &&
                ex.get_book("AAPL")->resting_order_count() == 2;
    printf(pass ? GREEN BOLD "PASS\n" RESET : RED BOLD "FAIL\n" RESET);
}

// =============================================================================
// TEST 6 — Exchange statistics
// =============================================================================
void test_exchange_stats() {
    divider("TEST 6: Exchange-Wide Statistics");

    printf("Concept: The Exchange tracks aggregate metrics across all symbols.\n");
    printf("These feed the performance dashboard and benchmarks later.\n\n");

    std::vector<ExecutionReport> fills;
    Exchange ex([&](const ExecutionReport& r){ fills.push_back(r); });
    ex.add_symbol("AAPL");
    ex.add_symbol("MSFT");

    // Submit, fill, cancel, modify across both symbols
    auto a1 = make_limit(Side::SELL, 100.00, 100, "AAPL", 1);
    auto a2 = make_limit(Side::BUY,  100.00,  60, "AAPL", 2);  // Crosses: fill 60
    auto m1 = make_limit(Side::SELL, 200.00, 200, "MSFT", 3);
    auto m2 = make_limit(Side::BUY,  200.00, 200, "MSFT", 4);  // Crosses: fill 200
    auto a3 = make_limit(Side::BUY,   99.00,  50, "AAPL", 5);  // Rests

    ex.submit_order(a1);
    ex.submit_order(a2);   // fill: 60 shares AAPL
    ex.submit_order(m1);
    ex.submit_order(m2);   // fill: 200 shares MSFT
    ex.submit_order(a3);

    ex.cancel_order(a3->order_id);       // 1 cancel
    ex.modify_order(a1->order_id, 100.00, 30); // 1 modify (qty down on remaining 40)

    ex.print_stats();

    const auto& s = ex.stats();
    printf("\n");

    // 5 submits
    bool pass = s.total_orders_submitted.load() == 5;
    printf("  submitted==5:  %s  (got %lu)\n",
           pass ? "PASS" : "FAIL",
           (unsigned long)s.total_orders_submitted.load());

    // 2 fills per match = 4 total (60+200 shares in 2 crossing events, 2 reports each)
    bool pass2 = s.total_fills.load() == 4;
    printf("  fills==4:      %s  (got %lu)\n",
           pass2 ? "PASS" : "FAIL",
           (unsigned long)s.total_fills.load());

    // Volume = 60 (AAPL) + 200 (MSFT) = 260 ... but each fill generates 2 reports
    // Each report carries exec_qty, so volume is counted twice (once per party)
    // Some exchanges count this as "single-sided" volume — we count all reports
    printf("  volume:        %lu shares (60 AAPL + 200 MSFT, each side)\n",
           (unsigned long)s.total_volume.load());

    bool pass3 = s.total_orders_cancelled.load() == 1;
    printf("  cancelled==1:  %s  (got %lu)\n",
           pass3 ? "PASS" : "FAIL",
           (unsigned long)s.total_orders_cancelled.load());

    bool pass4 = s.total_orders_modified.load() == 1;
    printf("  modified==1:   %s  (got %lu)\n\n",
           pass4 ? "PASS" : "FAIL",
           (unsigned long)s.total_orders_modified.load());

    bool all_pass = pass && pass2 && pass3 && pass4;
    printf(all_pass ? GREEN BOLD "PASS\n" RESET : RED BOLD "FAIL (check above)\n" RESET);
}

// =============================================================================
// main
// =============================================================================
int main() {
    printf("\n");
    printf(BOLD CYAN
        "+=====================================================+\n"
        "|  CPP MATCHING ENGINE -- Stage 2 Interactive Test  |\n"
        "|  Order Modification + Multi-Symbol + Exchange     |\n"
        "+=====================================================+\n"
        RESET "\n");

    test_multiple_symbols();
    test_modify_price();
    test_modify_qty_decrease();
    test_modify_qty_increase();
    test_cross_symbol_independence();
    test_exchange_stats();

    divider("SUMMARY");
    printf("Stage 2 adds:\n");
    printf("  Exchange orchestrator  (symbol registry, order routing)\n");
    printf("  Global order index     (cancel by order_id, no symbol needed)\n");
    printf("  Order modification     (3 cases: price change, qty up, qty down)\n");
    printf("  Multiple symbols       (AAPL, MSFT, TSLA all live simultaneously)\n");
    printf("  Exchange statistics    (orders, fills, volume, cancels, modifies)\n\n");
    printf("Combined with Stage 1, you now have a complete exchange kernel.\n");
    printf("Stages 3+ build trading agents on top of this.\n\n");

    return 0;
}
