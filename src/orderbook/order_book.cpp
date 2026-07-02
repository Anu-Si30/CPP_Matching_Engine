// =============================================================================
// order_book.cpp  —  Implementation of the OrderBook methods
// =============================================================================

#include "orderbook/order_book.h"
#include <stdexcept>

ObjectPool<Order> global_order_pool;

// ─── add_limit_order ──────────────────────────────────────────────────────────
//
// Inserts a resting limit order into the correct price level.
//
// How it works:
//   1. Determine which side (bids or asks) based on order->side
//   2. Find-or-create the PriceLevel at order->price  (O(log N))
//   3. Append the order to that level's deque           (O(1))
//   4. Register the order in the secondary index         (O(1))
//
void OrderBook::add_limit_order(std::shared_ptr<Order> order) {
    double price = order->price;
    uint64_t id  = order->order_id;

    if (order->side == Side::BUY) {
        // bids[price] returns a reference, creating the PriceLevel if it
        // doesn't exist (default-constructed with price=0.0, so we set it)
        auto& level = bids[price];
        level.price = price;
        level.add_order(order);
        order_index[id] = { Side::BUY, price };
    } else {
        auto& level = asks[price];
        level.price = price;
        level.add_order(order);
        order_index[id] = { Side::SELL, price };
    }
}

// ─── cancel_order ────────────────────────────────────────────────────────────
//
// Removes an order from the book.
//
// Complexity:
//   O(1) — secondary index lookup to find {side, price}
//   O(log N) — map lookup to find the price level
//   O(k) — linear scan within the level (k = orders at that price, typically small)
//
// We also ERASE empty price levels to keep the book clean.
// Critical: if you leave empty levels, best_bid() / best_ask() return stale prices.
//
bool OrderBook::cancel_order(uint64_t order_id) {
    auto idx_it = order_index.find(order_id);
    if (idx_it == order_index.end()) return false;  // Order not in this book

    Side   side  = idx_it->second.side;
    double price = idx_it->second.price;

    if (side == Side::BUY) {
        auto level_it = bids.find(price);
        if (level_it != bids.end()) {
            level_it->second.remove_order(order_id);
            if (level_it->second.empty()) {
                bids.erase(level_it);  // Clean up empty level
            }
        }
    } else {
        auto level_it = asks.find(price);
        if (level_it != asks.end()) {
            level_it->second.remove_order(order_id);
            if (level_it->second.empty()) {
                asks.erase(level_it);
            }
        }
    }

    order_index.erase(idx_it);
    return true;
}

// ─── bid_depth / ask_depth ────────────────────────────────────────────────────
//
// Returns a snapshot of the top N price levels for display purposes.
// Used by the terminal UI to draw the order book visualization.
//
std::vector<std::pair<double, uint64_t>> OrderBook::bid_depth(int levels) const {
    std::vector<std::pair<double, uint64_t>> result;
    result.reserve(levels);
    int count = 0;
    for (auto it = bids.begin(); it != bids.end(); ++it) {
        if (count++ >= levels) break;
        result.emplace_back(it->first, it->second.total_qty);
    }
    return result;  // Already sorted best->worst (std::greater comparator)
}

std::vector<std::pair<double, uint64_t>> OrderBook::ask_depth(int levels) const {
    std::vector<std::pair<double, uint64_t>> result;
    result.reserve(levels);
    int count = 0;
    for (auto it = asks.begin(); it != asks.end(); ++it) {
        if (count++ >= levels) break;
        result.emplace_back(it->first, it->second.total_qty);
    }
    return result;  // Already sorted best->worst (std::less comparator)
}
