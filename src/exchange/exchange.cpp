// =============================================================================
// exchange.cpp  —  Exchange orchestrator implementation
// =============================================================================

#include "exchange/exchange.h"
#include <cstdio>
#include <cstring>
#include <algorithm>

// ─── Constructor ──────────────────────────────────────────────────────────────
Exchange::Exchange(FillCallback cb)
    : engine_([this, cb](const ExecutionReport& report) {
        stats_.total_fills.fetch_add(1, std::memory_order_relaxed);
        stats_.total_volume.fetch_add(report.exec_qty, std::memory_order_relaxed);
        if (cb) cb(report);
    })
{}

// ─── add_symbol ───────────────────────────────────────────────────────────────
bool Exchange::add_symbol(const std::string& symbol) {
    if (books_.count(symbol)) return false;
    books_.emplace(symbol, OrderBook(symbol));
    return true;
}

bool Exchange::has_symbol(const std::string& symbol) const {
    return books_.count(symbol) > 0;
}

std::vector<std::string> Exchange::list_symbols() const {
    std::vector<std::string> result;
    result.reserve(books_.size());
    for (auto it = books_.begin(); it != books_.end(); ++it)
        result.push_back(it->first);
    std::sort(result.begin(), result.end());
    return result;
}

// ─── submit_order ─────────────────────────────────────────────────────────────
bool Exchange::submit_order(std::shared_ptr<Order> order) {
    // Look up the book for this symbol
    char sym_buf[9] = {};
    strncpy(sym_buf, order->symbol, 8);
    auto it = books_.find(std::string(sym_buf));
    if (it == books_.end()) {
        order->status = OrderStatus::REJECTED;
        stats_.total_rejects.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    stats_.total_orders_submitted.fetch_add(1, std::memory_order_relaxed);

    // Register limit orders in global index so cancel/modify can route them
    if (order->type == OrderType::LIMIT) {
        global_order_index_[order->order_id] = it->first;
    }

    engine_.submit_order(order, it->second);

    // If immediately fully filled, remove from global index (no longer resting)
    if (order->type == OrderType::LIMIT &&
        order->status == OrderStatus::FILLED) {
        global_order_index_.erase(order->order_id);
    }

    return true;
}

// ─── cancel_order ─────────────────────────────────────────────────────────────
bool Exchange::cancel_order(uint64_t order_id) {
    OrderBook* book = find_book_for_order(order_id);
    if (!book) return false;

    bool removed = engine_.cancel_order(order_id, *book);
    if (removed) {
        global_order_index_.erase(order_id);
        stats_.total_orders_cancelled.fetch_add(1, std::memory_order_relaxed);
    }
    return removed;
}

// ─── modify_order ─────────────────────────────────────────────────────────────
//
// Three cases:
//   A) Price changed OR qty increased → cancel + reinsert (lose queue position)
//   B) Qty decreased                  → in-place update   (keep queue position)
//   C) Nothing changed                → no-op
//
bool Exchange::modify_order(uint64_t order_id, double new_price, uint32_t new_qty) {
    OrderBook* book = find_book_for_order(order_id);
    if (!book) return false;

    auto idx_it = book->order_index.find(order_id);
    if (idx_it == book->order_index.end()) return false;

    Side   side      = idx_it->second.side;
    double old_price = idx_it->second.price;

    // Find the order object within the price level
    std::shared_ptr<Order> target;
    if (side == Side::BUY) {
        auto lit = book->bids.find(old_price);
        if (lit == book->bids.end()) return false;
        for (auto& o : lit->second.orders)
            if (o->order_id == order_id) { target = o; break; }
    } else {
        auto lit = book->asks.find(old_price);
        if (lit == book->asks.end()) return false;
        for (auto& o : lit->second.orders)
            if (o->order_id == order_id) { target = o; break; }
    }
    if (!target) return false;
    if (new_qty == 0) return false;  // Use cancel instead

    uint32_t current_remaining = target->remaining();
    bool price_changed = (new_price != old_price);
    bool qty_increased = (new_qty   >  current_remaining);
    bool qty_decreased = (new_qty   <  current_remaining);

    if (price_changed || qty_increased) {
        // ── Case A: Cancel + Reinsert ────────────────────────────────────────
        engine_.cancel_order(order_id, *book);

        // Restamp and update fields
        target->price        = new_price;
        target->filled_qty   = 0;
        target->quantity     = new_qty;
        target->timestamp_ns = now_ns();
        target->status       = OrderStatus::NEW;

        // Restore global index entry (same order_id, potentially new book-level)
        char sym[9] = {};
        strncpy(sym, target->symbol, 8);
        global_order_index_[order_id] = std::string(sym);

        // Resubmit — may cross and immediately fill
        engine_.submit_order(target, *book);

    } else if (qty_decreased) {
        // ── Case B: In-place qty reduction ───────────────────────────────────
        uint32_t reduction = current_remaining - new_qty;
        target->quantity   -= reduction;

        if (side == Side::BUY) {
            auto lit = book->bids.find(old_price);
            if (lit != book->bids.end())
                lit->second.update_qty(-static_cast<int64_t>(reduction));
        } else {
            auto lit = book->asks.find(old_price);
            if (lit != book->asks.end())
                lit->second.update_qty(-static_cast<int64_t>(reduction));
        }
    }
    // Case C: no-op if nothing changed

    stats_.total_orders_modified.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// ─── get_book ─────────────────────────────────────────────────────────────────
const OrderBook* Exchange::get_book(const std::string& symbol) const {
    auto it = books_.find(symbol);
    return (it == books_.end()) ? nullptr : &it->second;
}

OrderBook* Exchange::get_book(const std::string& symbol) {
    auto it = books_.find(symbol);
    return (it == books_.end()) ? nullptr : &it->second;
}

// ─── find_book_for_order ──────────────────────────────────────────────────────
OrderBook* Exchange::find_book_for_order(uint64_t order_id) {
    auto it = global_order_index_.find(order_id);
    if (it == global_order_index_.end()) return nullptr;
    auto bit = books_.find(it->second);
    if (bit == books_.end()) return nullptr;
    return &bit->second;
}

// ─── print_stats ─────────────────────────────────────────────────────────────
void Exchange::print_stats() const {
    printf("\n  Exchange Statistics\n");
    printf("  %-28s %lu\n", "Orders submitted:",
           (unsigned long)stats_.total_orders_submitted.load());
    printf("  %-28s %lu\n", "Orders cancelled:",
           (unsigned long)stats_.total_orders_cancelled.load());
    printf("  %-28s %lu\n", "Orders modified:",
           (unsigned long)stats_.total_orders_modified.load());
    printf("  %-28s %lu\n", "Fills generated:",
           (unsigned long)stats_.total_fills.load());
    printf("  %-28s %lu\n", "Volume (shares, per report):",
           (unsigned long)stats_.total_volume.load());
    printf("  %-28s %lu\n", "Rejected orders:",
           (unsigned long)stats_.total_rejects.load());
}
