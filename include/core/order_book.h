#pragma once
// =============================================================================
// order_book.h  —  The per-symbol order book
//
// The order book holds every resting (unmatched) limit order for one symbol.
// It is the central data structure of any exchange.
//
// Structure:
//   BID side: sorted highest→lowest (best bid first)
//   ASK side: sorted lowest→highest (best ask first)
//
// Within each price level, orders are in FIFO order (oldest first).
// This implements "price-time priority" — the universal matching rule.
// =============================================================================

#include "types.h"
#include <map>
#include <deque>
#include <unordered_map>
#include <vector>
#include <memory>
#include <string>
#include <functional>
#include <algorithm>

// ─── Price Level ─────────────────────────────────────────────────────────────
//
// All orders resting at a single price point.
// A deque gives us O(1) push_back (new orders) and O(1) pop_front (oldest fill).
//
// Why deque vs vector?
//   vector: erasing front is O(N) — has to shift every element left.
//   deque:  pop_front is O(1)     — uses a segmented array with a front pointer.
//
// Why deque vs list?
//   list: every node is a separate heap allocation — cache-unfriendly pointer chasing.
//   deque: contiguous segments — much better cache locality.
//
struct PriceLevel {
    double   price         = 0.0;
    uint64_t total_qty     = 0;     // Sum of remaining() across all orders at this level

    // Orders in FIFO order: front() is the oldest (highest priority to be matched)
    std::deque<std::shared_ptr<Order>> orders;

    // Add a new order to the back of the queue
    void add_order(std::shared_ptr<Order> order) {
        total_qty += order->remaining();
        orders.push_back(std::move(order));
    }

    // Remove an order by ID (for cancellation / full fills)
    // Returns true if found and removed
    bool remove_order(uint64_t order_id) {
        for (auto it = orders.begin(); it != orders.end(); ++it) {
            if ((*it)->order_id == order_id) {
                total_qty -= (*it)->remaining();
                orders.erase(it);
                return true;
            }
        }
        return false;
    }

    // After a partial fill, update total_qty to reflect the new remaining
    void update_qty(int64_t delta) {
        // delta is negative (qty decreased by fill amount)
        total_qty = static_cast<uint64_t>(
            static_cast<int64_t>(total_qty) + delta
        );
    }

    bool empty() const { return orders.empty(); }
};

// ─── OrderBook ────────────────────────────────────────────────────────────────
//
// Holds the complete two-sided book for ONE symbol.
// The MatchingEngine holds a reference to this and operates on it.
//
class OrderBook {
public:
    std::string symbol;

    // ── Sorted maps: price → PriceLevel ──────────────────────────────────────
    //
    // std::greater<double> on bids: highest price = begin() = best bid
    // std::less<double>    on asks: lowest  price = begin() = best ask
    //
    // We use std::map (a red-black tree) not unordered_map because:
    //   1. We need ORDERED iteration for the matching sweep
    //   2. best_bid() and best_ask() must be O(1) via begin()
    //   3. We need to walk levels in price-order to sweep fills
    //
    using BidMap = std::map<double, PriceLevel, std::greater<double>>;
    using AskMap = std::map<double, PriceLevel, std::less<double>>;

    BidMap bids;   // BUY  side: best bid = bids.begin()
    AskMap asks;   // SELL side: best ask = asks.begin()

    // ── Constructor ──────────────────────────────────────────────────────────
    explicit OrderBook(std::string sym) : symbol(std::move(sym)) {}

    // ── Adding orders to the book ────────────────────────────────────────────
    void add_limit_order(std::shared_ptr<Order> order);

    // ── Cancellation ─────────────────────────────────────────────────────────
    // Returns true if the order was found and removed
    bool cancel_order(uint64_t order_id);

    // ── Best price queries — O(1) ─────────────────────────────────────────────
    double best_bid() const {
        return bids.empty() ? 0.0 : bids.begin()->first;
    }
    double best_ask() const {
        return asks.empty() ? 0.0 : asks.begin()->first;
    }
    double mid_price() const {
        if (bids.empty() || asks.empty()) return 0.0;
        return (best_bid() + best_ask()) / 2.0;
    }
    double spread() const {
        if (bids.empty() || asks.empty()) return 0.0;
        return best_ask() - best_bid();
    }

    // ── Depth snapshots for display ──────────────────────────────────────────
    // Returns up to `levels` price points from the best price outward
    std::vector<std::pair<double, uint64_t>> bid_depth(int levels) const;
    std::vector<std::pair<double, uint64_t>> ask_depth(int levels) const;

    // ── Internal lookup — used by MatchingEngine ─────────────────────────────
    // Secondary index: order_id → {side, price}
    // This makes cancel O(1) lookup + O(log N) tree traversal instead of O(N) scan
    struct OrderLocation {
        Side   side;
        double price;
    };
    std::unordered_map<uint64_t, OrderLocation> order_index;

    // Stat: total number of orders currently resting
    size_t resting_order_count() const { return order_index.size(); }
};
