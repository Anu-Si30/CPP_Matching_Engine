#pragma once
// =============================================================================
// exchange.h  —  The top-level Exchange orchestrator
//
// This is the single entry point for ALL interactions with the exchange.
// Before Stage 2, the matching engine and order book were separate objects
// that had to be wired together manually in tests.
//
// The Exchange class:
//   1. Manages a registry of symbols (one OrderBook per symbol)
//   2. Routes incoming orders to the correct book
//   3. Exposes cancel and modify through a clean API
//   4. Tracks aggregate statistics across all symbols
//   5. Owns the MatchingEngine and its fill callback
//
// Why this matters architecturally:
//   Real exchanges have hundreds of symbols. NASDAQ handles ~3,000+.
//   You need one central object that knows "AAPL goes here, MSFT goes there."
//   Traders submit orders and cancel by order_id — they don't need to know
//   which book the order is in. The exchange routes it.
// =============================================================================

#include "core/order_book.h"
#include "core/matching_engine.h"
#include <unordered_map>
#include <string>
#include <vector>
#include <atomic>

// ─── Aggregate statistics across the whole exchange ──────────────────────────
struct ExchangeStats {
    std::atomic<uint64_t> total_orders_submitted{0};
    std::atomic<uint64_t> total_orders_cancelled{0};
    std::atomic<uint64_t> total_orders_modified{0};
    std::atomic<uint64_t> total_fills{0};
    std::atomic<uint64_t> total_volume{0};         // Shares traded
    std::atomic<uint64_t> total_rejects{0};        // Unknown symbol, etc.

    // Non-atomic for display (approximate is fine for stats)
    double total_notional = 0.0;  // Dollar value of all trades (price × qty)

    void print() const;
};

class Exchange {
public:
    using FillCallback = MatchingEngine::FillCallback;

    // ── Construction ──────────────────────────────────────────────────────────
    // Pass a callback that fires on every fill (for traders, risk system, UI)
    explicit Exchange(FillCallback cb);

    // ── Symbol management ─────────────────────────────────────────────────────
    // Add a tradeable symbol. Returns false if already exists.
    bool add_symbol(const std::string& symbol);

    // Is this symbol listed on this exchange?
    bool has_symbol(const std::string& symbol) const;

    // List all listed symbols
    std::vector<std::string> list_symbols() const;

    // ── Order submission ──────────────────────────────────────────────────────
    // Routes the order to the correct book. Returns false if symbol unknown.
    bool submit_order(std::shared_ptr<Order> order);

    // ── Cancellation ──────────────────────────────────────────────────────────
    // Find the order anywhere in the exchange and cancel it.
    // The global_order_index maps order_id → symbol so we know which book.
    bool cancel_order(uint64_t order_id);

    // ── Modification ──────────────────────────────────────────────────────────
    // Change the price and/or quantity of a resting order.
    //
    // Rules (same as real exchanges):
    //   Price change    → cancel + reinsert (lose queue position)
    //   Qty increase    → cancel + reinsert (lose queue position)
    //   Qty decrease    → modify in-place   (keep queue position — reward)
    //
    // Returns false if order_id not found or modification is invalid.
    bool modify_order(uint64_t order_id, double new_price, uint32_t new_qty);

    // ── Read access to books ──────────────────────────────────────────────────
    // Returns nullptr if symbol not found
    const OrderBook* get_book(const std::string& symbol) const;
    OrderBook*       get_book(const std::string& symbol);

    // ── Statistics ────────────────────────────────────────────────────────────
    const ExchangeStats& stats() const { return stats_; }
    void print_stats() const;

private:
    // One book per symbol
    std::unordered_map<std::string, OrderBook> books_;

    // Global order index: order_id → symbol
    // Needed so cancel/modify can find which book an order lives in
    // without scanning every book.
    std::unordered_map<uint64_t, std::string> global_order_index_;

    // The matching engine (stateless w.r.t. symbols — just needs a book ref)
    MatchingEngine engine_;

    // Live statistics
    ExchangeStats stats_;

    // NOTE: mutex will be added in Stage 6 (multithreading).
    // For now, the exchange is single-threaded.

    // Internal: find the order's book via global index
    OrderBook* find_book_for_order(uint64_t order_id);
};
