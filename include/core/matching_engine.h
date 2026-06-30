#pragma once
// =============================================================================
// matching_engine.h  —  The core matching logic
//
// The matching engine receives an incoming order and determines whether it
// crosses against any resting orders in the book. If it does, it generates
// ExecutionReports (fills) for both sides.
//
// Key rules implemented here:
//   1. Price-Time Priority (FIFO):
//      - Best price gets filled first
//      - Among equal prices, oldest order gets filled first
//   2. Aggressor pays the resting price:
//      - Fill always happens at the RESTING order's price, not the aggressor's
//   3. Market orders sweep until filled or book exhausted
//   4. Limit orders match until no more crossing, then REST in the book
// =============================================================================

#include "order_book.h"
#include <functional>

class MatchingEngine {
public:
    // FillCallback: called EVERY time a fill occurs.
    // The exchange calls this to notify traders, update PnL, etc.
    using FillCallback = std::function<void(const ExecutionReport&)>;

    explicit MatchingEngine(FillCallback cb);

    // ── Main entry point ─────────────────────────────────────────────────────
    // Routes the order to the appropriate matching logic, then rests any
    // unfilled remainder (for LIMIT orders) in the book.
    void submit_order(std::shared_ptr<Order> order, OrderBook& book);

    // ── Cancel an existing resting order ─────────────────────────────────────
    // Delegates to OrderBook::cancel_order and marks the order CANCELLED.
    bool cancel_order(uint64_t order_id, OrderBook& book);

    // ── Metrics ──────────────────────────────────────────────────────────────
    uint64_t total_orders_processed() const { return orders_processed; }
    uint64_t total_fills_generated()  const { return fills_generated; }
    uint64_t total_volume_traded()    const { return volume_traded; }

private:
    FillCallback on_fill;           // Invoked for every fill
    uint64_t     next_exec_id = 1;  // Monotonically increasing fill ID
    uint64_t     orders_processed = 0;
    uint64_t     fills_generated  = 0;
    uint64_t     volume_traded    = 0;

    // ── Core matching loops ───────────────────────────────────────────────────

    // LIMIT order: match while spread is crossed, then rest remainder
    void match_limit_order(std::shared_ptr<Order>& order, OrderBook& book);

    // MARKET order: sweep aggressively, ignore own price, match everything available
    void match_market_order(std::shared_ptr<Order>& order, OrderBook& book);

    // Execute a single fill between the incoming aggressor and a resting order.
    // Mutates both orders' filled_qty and generates two ExecutionReports (one per side).
    void execute_fill(
        std::shared_ptr<Order>& aggressor,
        std::shared_ptr<Order>& resting,
        double fill_price,
        OrderBook& book
    );
};
