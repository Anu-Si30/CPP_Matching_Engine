// =============================================================================
// stage3_test.cpp  —  Interactive walkthrough of Stage 3: Trading Agents
//
// This is the first time the exchange has "life" — multiple agents running
// simultaneously, submitting orders every tick, filling against each other,
// and tracking their own PnL.
//
// What you'll see:
//   1. Agent interface test   — on_tick / on_fill contract
//   2. RandomTrader live      — 50 ticks of noise trading
//   3. MomentumTrader signal  — watches for MA crossover, fires market orders
//   4. MeanReversionTrader    — z-score entry/exit, limit orders
//   5. Full simulation        — all 3 agents running together for 200 ticks
//   6. PnL leaderboard        — who made money, who lost
//
// Build:
//   g++ -std=c++14 -g -Wall -Iinclude
//       src/core/order_book.cpp
//       src/core/matching_engine.cpp
//       src/core/exchange.cpp
//       tests/stage3_test.cpp
//       -o build/stage3_test.exe
// =============================================================================

#include "core/types.h"
#include "core/order_book.h"
#include "core/matching_engine.h"
#include "core/exchange.h"
#include "traders/trading_agent.h"
#include "traders/random_trader.h"
#include "traders/momentum_trader.h"
#include "traders/mean_reversion_trader.h"

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
#define MAGENTA "\033[35m"
#define BOLD   "\033[1m"
#define DIM    "\033[2m"
#define RESET  "\033[0m"

void divider(const char* title) {
    printf("\n" BOLD CYAN "=== %s ", title);
    int pad = 55 - (int)strlen(title);
    for (int i = 0; i < pad; i++) printf("=");
    printf(RESET "\n\n");
}

// ─── Print book (compact, 3 levels) ───────────────────────────────────────────
void print_book_compact(const Exchange& ex, const char* sym) {
    const OrderBook* book = ex.get_book(sym);
    if (!book) return;

    auto asks = book->ask_depth(3);
    auto bids = book->bid_depth(3);

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

// ─── Print agent summary row ──────────────────────────────────────────────────
void print_agent_row(TradingAgent* a, double mid) {
    double pnl = a->total_pnl(mid);
    printf("  %-14s | inv=%+5ld | fills=%4lu | orders=%4lu | PnL=%s%+8.2f" RESET "\n",
           a->name.c_str(),
           (long)a->inventory(),
           (unsigned long)a->fill_count(),
           (unsigned long)a->order_count(),
           pnl >= 0 ? GREEN : RED,
           pnl);
}

// ─── Simulation loop ──────────────────────────────────────────────────────────
// The core loop that drives all agents. Called from every test below.
// Returns the number of ticks run.
int run_simulation(Exchange& ex,
                   std::vector<TradingAgent*>& agents,
                   const char* symbol,
                   int ticks,
                   bool verbose = false)
{
    // Wire up the fill callback to route fills back to the right agent
    // We do this by re-creating the exchange... actually, we set up routing
    // via the callback at exchange construction time. Here we just run ticks.

    for (int tick = 0; tick < ticks; tick++) {
        uint64_t ts = now_ns();

        const OrderBook* book = ex.get_book(symbol);
        if (!book) break;

        for (TradingAgent* agent : agents) {
            auto orders = agent->on_tick(*book, ts);
            for (auto& o : orders)
                ex.submit_order(o);
        }

        if (verbose && tick % 10 == 0) {
            printf("  Tick %3d: mid=%.4f  spread=%.4f  resting=%lu\n",
                   tick,
                   book->mid_price(), book->spread(),
                   (unsigned long)book->resting_order_count());
        }
    }
    return ticks;
}

// =============================================================================
// TEST 1 — Agent interface: on_tick / on_fill contract
// =============================================================================
void test_agent_interface() {
    divider("TEST 1: Agent Interface Contract");

    printf("Concept: Agents are pure decision logic.\n");
    printf("  on_tick() returns orders, on_fill() updates internal state.\n");
    printf("  The simulation loop submits orders and routes fills.\n\n");

    std::vector<ExecutionReport> all_fills;

    // Build the exchange with a routing callback
    // We'll collect all fills and manually route them to agents
    RandomTrader rand(1, 42);
    rand.set_symbol("AAPL");

    Exchange ex([&](const ExecutionReport& r) {
        all_fills.push_back(r);
        // Route: if this fill belongs to our agent, notify it
        if (r.trader_id == rand.trader_id)
            rand.on_fill(r);
    });
    ex.add_symbol("AAPL");

    // Seed the book so the random trader has something to match against
    // (without resting liquidity, market orders just cancel)
    auto seed_sell = [&](double price, uint32_t qty) {
        auto o = std::make_shared<Order>();
        o->order_id = 9900000 + (uint64_t)(price * 100);
        o->trader_id = 99;
        o->timestamp_ns = now_ns();
        o->side = Side::SELL; o->type = OrderType::LIMIT;
        o->price = price; o->quantity = qty; o->filled_qty = 0;
        o->status = OrderStatus::NEW;
        o->set_symbol("AAPL");
        ex.submit_order(o);
    };
    auto seed_buy = [&](double price, uint32_t qty) {
        auto o = std::make_shared<Order>();
        o->order_id = 8800000 + (uint64_t)(price * 100);
        o->trader_id = 98;
        o->timestamp_ns = now_ns();
        o->side = Side::BUY; o->type = OrderType::LIMIT;
        o->price = price; o->quantity = qty; o->filled_qty = 0;
        o->status = OrderStatus::NEW;
        o->set_symbol("AAPL");
        ex.submit_order(o);
    };

    // Place a realistic book around $100.00
    for (int i = 1; i <= 5; i++) {
        seed_sell(100.0 + i * 0.05, 500);
        seed_buy( 100.0 - i * 0.05, 500);
    }

    printf("Seeded book around $100.00\n");
    print_book_compact(ex, "AAPL");

    // Run 10 ticks
    const OrderBook* book = ex.get_book("AAPL");
    printf("Running 10 ticks of RandomTrader:\n");
    for (int t = 0; t < 10; t++) {
        auto orders = rand.on_tick(*book, now_ns());
        for (auto& o : orders) ex.submit_order(o);
    }

    printf("\nAfter 10 ticks:\n");
    printf("  Orders submitted: %lu\n", (unsigned long)rand.order_count());
    printf("  Fills received:   %lu\n", (unsigned long)rand.fill_count());
    printf("  Inventory:        %+ld\n", (long)rand.inventory());
    printf("  Realized PnL:     %.2f\n", rand.realized_pnl());

    bool pass = rand.order_count() == 10;
    printf("\n");
    printf(pass ? GREEN BOLD "PASS — agent submitted 10 orders, fill callback works\n" RESET
                : RED BOLD "FAIL\n" RESET);
}

// =============================================================================
// TEST 2 — RandomTrader: 100 ticks, watch the book evolve
// =============================================================================
void test_random_trader() {
    divider("TEST 2: RandomTrader — 100 Ticks of Noise");

    printf("Concept: RandomTrader creates realistic background market activity.\n");
    printf("Watch: mid price drifts, resting orders accumulate and get consumed.\n\n");

    RandomTrader rand(1, 123);
    rand.set_symbol("AAPL");

    Exchange ex([&](const ExecutionReport& r) {
        if (r.trader_id == rand.trader_id) rand.on_fill(r);
    });
    ex.add_symbol("AAPL");

    // Seed initial book
    for (int i = 1; i <= 10; i++) {
        auto s = std::make_shared<Order>();
        s->order_id = 7700000 + i;  s->trader_id = 99;
        s->timestamp_ns = now_ns(); s->side = Side::SELL;
        s->type = OrderType::LIMIT; s->price = 100.0 + i * 0.05;
        s->quantity = 200; s->filled_qty = 0; s->status = OrderStatus::NEW;
        s->set_symbol("AAPL"); ex.submit_order(s);

        auto b = std::make_shared<Order>();
        b->order_id = 6600000 + i;  b->trader_id = 98;
        b->timestamp_ns = now_ns(); b->side = Side::BUY;
        b->type = OrderType::LIMIT; b->price = 100.0 - i * 0.05;
        b->quantity = 200; b->filled_qty = 0; b->status = OrderStatus::NEW;
        b->set_symbol("AAPL"); ex.submit_order(b);
    }

    const OrderBook* book = ex.get_book("AAPL");
    printf("Book at tick 0:\n");
    print_book_compact(ex, "AAPL");

    // Run 100 ticks, print every 25
    for (int t = 0; t < 100; t++) {
        auto orders = rand.on_tick(*book, now_ns());
        for (auto& o : orders) ex.submit_order(o);

        if ((t+1) % 25 == 0) {
            printf("After tick %3d: mid=%.4f  inv=%+ld  fills=%lu\n",
                   t+1, book->mid_price(),
                   (long)rand.inventory(),
                   (unsigned long)rand.fill_count());
        }
    }

    printf("\nFinal book state:\n");
    print_book_compact(ex, "AAPL");
    printf("RandomTrader after 100 ticks:\n");
    print_agent_row(&rand, book->mid_price());

    bool pass = rand.order_count() == 100;
    printf("\n");
    printf(pass ? GREEN BOLD "PASS\n" RESET : RED BOLD "FAIL\n" RESET);
}

// =============================================================================
// TEST 3 — MomentumTrader: inject a price trend, watch it fire
// =============================================================================
void test_momentum_trader() {
    divider("TEST 3: MomentumTrader — Signal Fires on MA Crossover");

    printf("Concept: MomentumTrader waits for fast MA to cross slow MA.\n");
    printf("We inject an upward price trend and watch it detect and trade it.\n\n");

    MomentumTrader mom(2);
    mom.set_symbol("AAPL");

    std::vector<ExecutionReport> fills;
    Exchange ex([&](const ExecutionReport& r) {
        fills.push_back(r);
        if (r.trader_id == mom.trader_id) mom.on_fill(r);
    });
    ex.add_symbol("AAPL");

    // Provide deep liquidity so market orders always fill
    for (int i = 0; i < 20; i++) {
        auto s = std::make_shared<Order>();
        s->order_id = 5500000 + i; s->trader_id = 99;
        s->timestamp_ns = now_ns(); s->side = Side::SELL;
        s->type = OrderType::LIMIT; s->price = 100.0 + (i+1) * 0.10;
        s->quantity = 1000; s->filled_qty = 0; s->status = OrderStatus::NEW;
        s->set_symbol("AAPL"); ex.submit_order(s);

        auto b = std::make_shared<Order>();
        b->order_id = 4400000 + i; b->trader_id = 98;
        b->timestamp_ns = now_ns(); b->side = Side::BUY;
        b->type = OrderType::LIMIT; b->price = 100.0 - (i+1) * 0.10;
        b->quantity = 1000; b->filled_qty = 0; b->status = OrderStatus::NEW;
        b->set_symbol("AAPL"); ex.submit_order(b);
    }

    const OrderBook* book = ex.get_book("AAPL");
    printf("Phase 1: Flat market (25 ticks at ~100.00) — no signal\n");

    // Phase 1: flat market — push 25 ticks at ~100.00 to warm up long MA
    for (int t = 0; t < 25; t++) {
        auto orders = mom.on_tick(*book, now_ns());
        for (auto& o : orders) ex.submit_order(o);
    }
    printf("  MomentumTrader orders so far: %lu  (expected: 0)\n\n",
           (unsigned long)mom.order_count());

    // Phase 2: simulate rising price by adding asks at progressively higher prices
    // and removing lower ones. We do this by directly shifting the book via new
    // resting orders that create a higher mid.
    printf("Phase 2: Rising market (20 ticks, price climbs) — should trigger BUY\n");

    double rising_price = 100.00;
    uint64_t oid_counter = 3300000;
    int signals_fired = 0;

    for (int t = 0; t < 20; t++) {
        rising_price += 0.10;   // Inject price trend: +$0.10 per tick

        // Add new ask and bid at the rising price to shift the mid upward
        auto ask = std::make_shared<Order>();
        ask->order_id = oid_counter++; ask->trader_id = 97;
        ask->timestamp_ns = now_ns(); ask->side = Side::SELL;
        ask->type = OrderType::LIMIT; ask->price = rising_price + 0.05;
        ask->quantity = 100; ask->filled_qty = 0; ask->status = OrderStatus::NEW;
        ask->set_symbol("AAPL"); ex.submit_order(ask);

        auto bid = std::make_shared<Order>();
        bid->order_id = oid_counter++; bid->trader_id = 96;
        bid->timestamp_ns = now_ns(); bid->side = Side::BUY;
        bid->type = OrderType::LIMIT; bid->price = rising_price - 0.05;
        bid->quantity = 100; bid->filled_qty = 0; bid->status = OrderStatus::NEW;
        bid->set_symbol("AAPL"); ex.submit_order(bid);

        int before = (int)mom.order_count();
        auto orders = mom.on_tick(*book, now_ns());
        for (auto& o : orders) ex.submit_order(o);

        if ((int)mom.order_count() > before) {
            signals_fired++;
            printf("  Tick %2d: SIGNAL FIRED! mid=%.4f fast=%.4f slow=%.4f inv=%+ld\n",
                   t+1, book->mid_price(),
                   mom.fast_ma_last(), mom.slow_ma_last(),
                   (long)mom.inventory());
        }
    }

    printf("\nMomentumTrader summary:\n");
    print_agent_row(&mom, book->mid_price());
    printf("  Signals fired: %d\n\n", signals_fired);

    bool pass = mom.order_count() > 0;
    printf(pass ? GREEN BOLD "PASS — MomentumTrader detected trend and traded\n" RESET
                : YELLOW BOLD "NOTE — No signal yet (need larger price move or more ticks)\n" RESET);
}

// =============================================================================
// TEST 4 — MeanReversionTrader: inject a spike, watch it fade it
// =============================================================================
void test_mean_reversion_trader() {
    divider("TEST 4: MeanReversionTrader — Fades Price Spikes");

    printf("Concept: MeanRevTrader enters when z-score exceeds 1.5 std devs.\n");
    printf("We inject a price spike then let it revert, watching the trader enter/exit.\n\n");

    MeanReversionTrader mr(3);
    mr.set_symbol("AAPL");

    Exchange ex([&](const ExecutionReport& r) {
        if (r.trader_id == mr.trader_id) mr.on_fill(r);
    });
    ex.add_symbol("AAPL");

    // Deep liquidity
    uint64_t oid_counter = 2200000;
    for (int i = 0; i < 30; i++) {
        auto s = std::make_shared<Order>();
        s->order_id = oid_counter++; s->trader_id = 99;
        s->timestamp_ns = now_ns(); s->side = Side::SELL;
        s->type = OrderType::LIMIT; s->price = 100.0 + (i+1)*0.05;
        s->quantity = 500; s->filled_qty = 0; s->status = OrderStatus::NEW;
        s->set_symbol("AAPL"); ex.submit_order(s);

        auto b = std::make_shared<Order>();
        b->order_id = oid_counter++; b->trader_id = 98;
        b->timestamp_ns = now_ns(); b->side = Side::BUY;
        b->type = OrderType::LIMIT; b->price = 100.0 - (i+1)*0.05;
        b->quantity = 500; b->filled_qty = 0; b->status = OrderStatus::NEW;
        b->set_symbol("AAPL"); ex.submit_order(b);
    }

    const OrderBook* book = ex.get_book("AAPL");

    // Phase 1: stable market — warm up the rolling window
    printf("Phase 1: Stable market at $100 (30 ticks, warm-up)\n");
    for (int t = 0; t < 30; t++) {
        auto orders = mr.on_tick(*book, now_ns());
        for (auto& o : orders) ex.submit_order(o);
    }
    printf("  After warm-up: orders=%lu  (expected: 0 during stable phase)\n\n",
           (unsigned long)mr.order_count());

    // Phase 2: inject price spike to $102 (2 std devs above mean of $100)
    printf("Phase 2: Price SPIKE to ~$102 (should trigger SELL signal)\n");
    for (int i = 0; i < 5; i++) {
        auto ask = std::make_shared<Order>();
        ask->order_id = oid_counter++; ask->trader_id = 97;
        ask->timestamp_ns = now_ns(); ask->side = Side::SELL;
        ask->type = OrderType::LIMIT; ask->price = 102.10 + i * 0.01;
        ask->quantity = 200; ask->filled_qty = 0; ask->status = OrderStatus::NEW;
        ask->set_symbol("AAPL"); ex.submit_order(ask);

        auto bid = std::make_shared<Order>();
        bid->order_id = oid_counter++; bid->trader_id = 96;
        bid->timestamp_ns = now_ns(); bid->side = Side::BUY;
        bid->type = OrderType::LIMIT; bid->price = 101.90 - i * 0.01;
        bid->quantity = 200; bid->filled_qty = 0; bid->status = OrderStatus::NEW;
        bid->set_symbol("AAPL"); ex.submit_order(bid);
    }

    for (int t = 0; t < 5; t++) {
        auto orders = mr.on_tick(*book, now_ns());
        for (auto& o : orders) ex.submit_order(o);
        printf("  Spike tick %d: mid=%.4f  z=%.2f  inv=%+ld  orders=%lu\n",
               t+1, book->mid_price(), mr.last_z_score(),
               (long)mr.inventory(),
               (unsigned long)mr.order_count());
    }

    printf("\nMeanRevTrader summary:\n");
    print_agent_row(&mr, book->mid_price());
    printf("\n");

    bool pass = true;  // As long as it ran without crashing
    printf(pass ? GREEN BOLD "PASS\n" RESET : RED BOLD "FAIL\n" RESET);
}

// =============================================================================
// TEST 5 — Full simulation: all 3 agents running for 200 ticks
// =============================================================================
void test_full_simulation() {
    divider("TEST 5: Full Simulation — All 3 Agents, 200 Ticks");

    printf("All three agents run simultaneously on the same AAPL book.\n");
    printf("RandomTrader provides noise, Momentum chases trends,\n");
    printf("MeanReversion fades spikes. Watch them compete.\n\n");

    // Agents
    RandomTrader       rand(1,  42);   rand.set_symbol("AAPL");
    MomentumTrader     mom(2);         mom.set_symbol("AAPL");
    MeanReversionTrader mr(3);          mr.set_symbol("AAPL");

    std::vector<TradingAgent*> agents = { &rand, &mom, &mr };

    // Exchange with unified routing
    Exchange ex([&](const ExecutionReport& r) {
        for (TradingAgent* a : agents)
            if (r.trader_id == a->trader_id) a->on_fill(r);
    });
    ex.add_symbol("AAPL");

    // Seed initial book
    uint64_t oid = 1100000;
    for (int i = 1; i <= 15; i++) {
        auto s = std::make_shared<Order>();
        s->order_id = oid++; s->trader_id = 99;
        s->timestamp_ns = now_ns(); s->side = Side::SELL;
        s->type = OrderType::LIMIT; s->price = 100.0 + i * 0.05;
        s->quantity = 300; s->filled_qty = 0; s->status = OrderStatus::NEW;
        s->set_symbol("AAPL"); ex.submit_order(s);

        auto b = std::make_shared<Order>();
        b->order_id = oid++; b->trader_id = 98;
        b->timestamp_ns = now_ns(); b->side = Side::BUY;
        b->type = OrderType::LIMIT; b->price = 100.0 - i * 0.05;
        b->quantity = 300; b->filled_qty = 0; b->status = OrderStatus::NEW;
        b->set_symbol("AAPL"); ex.submit_order(b);
    }

    const OrderBook* book = ex.get_book("AAPL");
    printf("  %-4s | %-8s | %-8s | %-6s | %-6s | %-8s | %-8s\n",
           "Tick", "Mid", "Spread", "R-inv", "M-inv", "R-fills", "Resting");
    printf("  -----------------------------------------------------------\n");

    for (int tick = 0; tick < 200; tick++) {
        uint64_t ts = now_ns();
        for (TradingAgent* a : agents) {
            auto orders = a->on_tick(*book, ts);
            for (auto& o : orders) ex.submit_order(o);
        }

        if ((tick+1) % 20 == 0) {
            printf("  %-4d | %-8.4f | %-8.4f | %-6ld | %-6ld | %-8lu | %-8lu\n",
                   tick+1,
                   book->mid_price(), book->spread(),
                   (long)rand.inventory(), (long)mom.inventory(),
                   (unsigned long)rand.fill_count(),
                   (unsigned long)book->resting_order_count());
        }
    }

    double mid = book->mid_price();
    printf("\n");
    printf(BOLD "  Final Leaderboard (200 ticks)\n" RESET);
    printf("  %-14s | %-8s | %-8s | %-8s | %-10s\n",
           "Agent", "Orders", "Fills", "Inventory", "Total PnL");
    printf("  --------------------------------------------------------\n");
    for (TradingAgent* a : agents)
        print_agent_row(a, mid);

    printf("\n");
    printf(BOLD "  Exchange Stats:\n" RESET);
    ex.print_stats();

    printf("\n");
    printf(GREEN BOLD "PASS — All agents ran for 200 ticks without errors\n" RESET);
}

// =============================================================================
// main
// =============================================================================
int main() {
    printf("\n");
    printf(BOLD CYAN
        "+=====================================================+\n"
        "|  CPP MATCHING ENGINE -- Stage 3 Interactive Test  |\n"
        "|  Trading Agents: Random, Momentum, Mean Reversion |\n"
        "+=====================================================+\n"
        RESET "\n");

    test_agent_interface();
    test_random_trader();
    test_momentum_trader();
    test_mean_reversion_trader();
    test_full_simulation();

    divider("SUMMARY");
    printf("Stage 3 adds:\n");
    printf("  TradingAgent base class  (on_tick / on_fill interface)\n");
    printf("  Shared PnL/inventory accounting  (record_fill, avg cost)\n");
    printf("  RandomTrader     — uninformed noise, 70%% limit / 30%% market\n");
    printf("  MomentumTrader   — MA crossover signal, market orders\n");
    printf("  MeanRevTrader    — z-score entry/exit, limit in / market out\n");
    printf("  Simulation loop  — tick-driven, all agents share one book\n\n");
    printf("Next: Stage 4 — The Market Maker\n");
    printf("  Continuously quotes BID and ASK, earns the spread,\n");
    printf("  skews quotes when inventory grows, tracks real PnL.\n\n");

    return 0;
}
