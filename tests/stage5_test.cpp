// =============================================================================
// stage5_test.cpp  —  Interactive walkthrough of Stage 5: Risk Engine
//
// Build:
//   g++ -std=c++14 -g -Wall -Isrc
//       src/orderbook/order_book.cpp
//       src/matching/matching_engine.cpp
//       src/exchange/exchange.cpp
//       tests/stage5_test.cpp
//       -o build/stage5_test.exe
// =============================================================================

#include "orderbook/types.h"
#include "orderbook/order_book.h"
#include "matching/matching_engine.h"
#include "exchange/exchange.h"
#include "risk/risk_engine.h"

#include <cstdio>
#include <cstring>
#include <vector>
#include <memory>

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

// =============================================================================
// TEST 1 — Pre-Trade Limit Check (Open Exposure)
// =============================================================================
void test_open_exposure() {
    divider("TEST 1: Pre-Trade Check on Open Exposure");

    printf("Concept: A trader can only hold 1,000 shares MAX.\n");
    printf("Resting orders count towards this limit (open exposure).\n\n");

    RiskEngine risk;
    RiskLimits limits;
    limits.max_long_position = 1000;
    limits.max_short_position = 1000;
    
    uint64_t TRADER_ID = 42;
    risk.set_limits(TRADER_ID, "AAPL", limits);

    printf("Trader limits set: max_long = 1000, max_short = 1000\n\n");

    // 1. Submit BUY for 800 shares
    auto o1 = make_limit(Side::BUY, 100.0, 800, "AAPL", TRADER_ID);
    bool ok1 = risk.check_order(o1);
    printf("1. Check BUY 800 shares -> %s\n", ok1 ? "APPROVED" : "REJECTED");
    
    // 2. Submit BUY for 300 shares (Total = 1100 > 1000)
    auto o2 = make_limit(Side::BUY, 99.0, 300, "AAPL", TRADER_ID);
    bool ok2 = risk.check_order(o2);
    printf("2. Check BUY 300 shares -> %s\n", ok2 ? "APPROVED" : "REJECTED");
    printf("   (Order status: %s)\n", o2->status == OrderStatus::REJECTED ? "REJECTED" : "OTHER");

    // 3. Submit SELL for 500 shares
    auto o3 = make_limit(Side::SELL, 101.0, 500, "AAPL", TRADER_ID);
    bool ok3 = risk.check_order(o3);
    printf("3. Check SELL 500 shares -> %s\n", ok3 ? "APPROVED" : "REJECTED");

    auto state = risk.get_state(TRADER_ID, "AAPL");
    printf("\nRisk State for TRADER_ID 42:\n");
    printf("  Realized Position: %ld\n", (long)state.position);
    printf("  Open Buy Qty:      %ld (expected 800)\n", (long)state.open_buy_qty);
    printf("  Open Sell Qty:     %ld (expected 500)\n\n", (long)state.open_sell_qty);

    bool pass = (ok1 == true) && (ok2 == false) && (ok3 == true) && 
                (state.open_buy_qty == 800) && (state.open_sell_qty == 500);
    printf(pass ? GREEN BOLD "PASS\n" RESET : RED BOLD "FAIL\n" RESET);
}

// =============================================================================
// TEST 2 — Fills converting Open Exposure to Position
// =============================================================================
void test_fill_conversion() {
    divider("TEST 2: Fills converting Open Exposure to Position");

    printf("Concept: When an order is filled, open exposure decreases and\n");
    printf("realized position increases. The total risk remains the same.\n\n");

    RiskEngine risk;
    RiskLimits limits;
    limits.max_long_position = 1000;
    limits.max_short_position = 1000;
    uint64_t TRADER_ID = 42;
    risk.set_limits(TRADER_ID, "AAPL", limits);

    // Initial state: 1000 open buy
    auto o1 = make_limit(Side::BUY, 100.0, 1000, "AAPL", TRADER_ID);
    risk.check_order(o1);

    auto state1 = risk.get_state(TRADER_ID, "AAPL");
    printf("Before fill: Pos=%ld, OpenBuy=%ld\n", (long)state1.position, (long)state1.open_buy_qty);

    // Simulate partial fill of 400 shares
    ExecutionReport r;
    r.order_id = o1->order_id;
    r.trader_id = TRADER_ID;
    r.side = Side::BUY;
    r.exec_qty = 400;
    r.exec_price = 100.0;
    strncpy(r.symbol, "AAPL", 8);

    risk.on_fill(r);
    auto state2 = risk.get_state(TRADER_ID, "AAPL");
    printf("After partial fill (400): Pos=%ld, OpenBuy=%ld (expected: Pos=400, OpenBuy=600)\n", 
           (long)state2.position, (long)state2.open_buy_qty);

    // Try to buy 500 more -> should fail because Pos(400) + OpenBuy(600) + New(500) = 1500 > 1000
    auto o2 = make_limit(Side::BUY, 99.0, 500, "AAPL", TRADER_ID);
    bool ok2 = risk.check_order(o2);
    printf("Try to buy 500 more -> %s (expected REJECTED)\n", ok2 ? "APPROVED" : "REJECTED");

    bool pass = (state2.position == 400) && (state2.open_buy_qty == 600) && (ok2 == false);
    printf("\n");
    printf(pass ? GREEN BOLD "PASS\n" RESET : RED BOLD "FAIL\n" RESET);
}

// =============================================================================
// TEST 3 — Cancelling frees up Risk Limits
// =============================================================================
void test_cancel_frees_risk() {
    divider("TEST 3: Cancelling Orders frees up Risk Limit");

    printf("Concept: When a resting order is cancelled, its open exposure is\n");
    printf("cleared, freeing up the limit for new orders.\n\n");

    RiskEngine risk;
    RiskLimits limits;
    limits.max_long_position = 1000;
    limits.max_short_position = 1000;
    uint64_t TRADER_ID = 42;
    risk.set_limits(TRADER_ID, "AAPL", limits);

    // Use full limit
    auto o1 = make_limit(Side::BUY, 100.0, 1000, "AAPL", TRADER_ID);
    risk.check_order(o1);

    auto o2 = make_limit(Side::BUY, 100.0, 1, "AAPL", TRADER_ID);
    bool ok2 = risk.check_order(o2);
    printf("Initial BUY 1 -> %s (Limit full)\n", ok2 ? "APPROVED" : "REJECTED");

    // Cancel the 1000-share order
    printf("Cancelling the 1000-share order...\n");
    risk.on_cancel(o1->order_id);

    auto state = risk.get_state(TRADER_ID, "AAPL");
    printf("OpenBuy after cancel: %ld\n", (long)state.open_buy_qty);

    // Try again
    bool ok3 = risk.check_order(o2);
    printf("Try BUY 1 again -> %s (Should be approved now)\n", ok3 ? "APPROVED" : "REJECTED");

    bool pass = (ok2 == false) && (state.open_buy_qty == 0) && (ok3 == true);
    printf("\n");
    printf(pass ? GREEN BOLD "PASS\n" RESET : RED BOLD "FAIL\n" RESET);
}

int main() {
    printf("\n");
    printf(BOLD CYAN
        "+=====================================================+\n"
        "|  CPP MATCHING ENGINE -- Stage 5 Interactive Test  |\n"
        "|  Risk Engine (Pre-Trade Checks & Limits)          |\n"
        "+=====================================================+\n"
        RESET "\n");

    test_open_exposure();
    test_fill_conversion();
    test_cancel_frees_risk();

    divider("SUMMARY");
    printf("Stage 5 adds:\n");
    printf("  RiskEngine class — sits before the Exchange\n");
    printf("  Pre-trade limit checks (max long, max short)\n");
    printf("  Open Exposure tracking (resting orders count against risk)\n");
    printf("  Fill & Cancel integration to maintain real-time position limits\n\n");
    
    return 0;
}
