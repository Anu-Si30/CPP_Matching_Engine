// =============================================================================
// matching_engine.cpp  —  The heart of the exchange
// =============================================================================

#include "matching/matching_engine.h"
#include <cstring>
#include <algorithm>

// ─── Constructor ──────────────────────────────────────────────────────────────
MatchingEngine::MatchingEngine(FillCallback cb)
    : on_fill(std::move(cb)) {}

// ─── submit_order — main entry point ─────────────────────────────────────────
//
// Decides whether to call match_limit_order or match_market_order.
// After matching, any unfilled LIMIT order remainder rests in the book.
// MARKET orders do NOT rest — they are either filled or rejected.
//
void MatchingEngine::submit_order(std::shared_ptr<Order> order, OrderBook& book) {
    orders_processed++;
    order->status = OrderStatus::NEW;

    if (order->tif == TimeInForce::FOK) {
        if (!can_fill_completely(order, book)) {
            order->status = OrderStatus::CANCELLED;
            return;
        }
    }

    if (order->type == OrderType::MARKET) {
        match_market_order(order, book);
        // Whatever wasn't filled on a market order is simply discarded
        if (order->remaining() > 0) {
            order->status = OrderStatus::CANCELLED;
        }
    } else {
        // LIMIT order
        match_limit_order(order, book);
        
        // Post remainder to book ONLY if it's GTC
        if (order->is_active() && order->remaining() > 0) {
            if (order->tif == TimeInForce::IOC || order->tif == TimeInForce::FOK) {
                order->status = OrderStatus::CANCELLED;
            } else {
                book.add_limit_order(order);
            }
        }
    }
}

// ─── cancel_order ─────────────────────────────────────────────────────────────
bool MatchingEngine::cancel_order(uint64_t order_id, OrderBook& book) {
    return book.cancel_order(order_id);
}

// ─── match_limit_order ────────────────────────────────────────────────────────
//
// A LIMIT BUY at price P can match any resting SELL at price ≤ P.
// A LIMIT SELL at price P can match any resting BUY  at price ≥ P.
//
// We sweep through the OPPOSITE side of the book level by level,
// filling FIFO within each level, until either:
//   (a) the incoming order is completely filled, or
//   (b) the spread is no longer crossed (no more eligible resting orders)
//
// The fill price is ALWAYS the resting order's price (not the aggressor's).
// This is the standard exchange rule and rewards passive liquidity providers.
//
void MatchingEngine::match_limit_order(std::shared_ptr<Order>& order, OrderBook& book) {
    if (order->side == Side::BUY) {
        // ── BUY: match against resting ASKS ──────────────────────────────────
        // asks is sorted low→high, so begin() is the cheapest ask (best for us)
        while (order->remaining() > 0 && !book.asks.empty()) {
            auto& best_level = book.asks.begin()->second;
            double ask_price = best_level.price;

            // Spread check: if cheapest ask > our limit price, stop
            if (ask_price > order->price) break;

            // Fill orders within this level, FIFO
            while (order->remaining() > 0 && !best_level.orders.empty()) {
                auto& resting = best_level.orders.front();
                execute_fill(order, resting, ask_price, book);
            }

            // If the level is now empty, remove it from the book
            if (best_level.empty()) {
                book.asks.erase(book.asks.begin());
            }
        }
    } else {
        // ── SELL: match against resting BIDS ─────────────────────────────────
        // bids is sorted high→low, so begin() is the highest bid (best for us)
        while (order->remaining() > 0 && !book.bids.empty()) {
            auto& best_level = book.bids.begin()->second;
            double bid_price = best_level.price;

            // Spread check: if highest bid < our limit price, stop
            if (bid_price < order->price) break;

            while (order->remaining() > 0 && !best_level.orders.empty()) {
                auto& resting = best_level.orders.front();
                execute_fill(order, resting, bid_price, book);
            }

            if (best_level.empty()) {
                book.bids.erase(book.bids.begin());
            }
        }
    }
}

// ─── match_market_order ───────────────────────────────────────────────────────
//
// Market orders have NO price limit — they sweep whatever is available.
// A market BUY takes from the ask side until filled or book is empty.
// A market SELL takes from the bid side until filled or book is empty.
//
// Risk: in a thin book, a market order can "walk the book" and fill at
// terrible prices. This is called "market impact" and is why professional
// traders rarely use market orders.
//
void MatchingEngine::match_market_order(std::shared_ptr<Order>& order, OrderBook& book) {
    if (order->side == Side::BUY) {
        while (order->remaining() > 0 && !book.asks.empty()) {
            auto& best_level = book.asks.begin()->second;
            double ask_price = best_level.price;

            while (order->remaining() > 0 && !best_level.orders.empty()) {
                auto& resting = best_level.orders.front();
                execute_fill(order, resting, ask_price, book);
            }

            if (best_level.empty()) {
                book.asks.erase(book.asks.begin());
            }
        }
    } else {
        while (order->remaining() > 0 && !book.bids.empty()) {
            auto& best_level = book.bids.begin()->second;
            double bid_price = best_level.price;

            while (order->remaining() > 0 && !best_level.orders.empty()) {
                auto& resting = best_level.orders.front();
                execute_fill(order, resting, bid_price, book);
            }

            if (best_level.empty()) {
                book.bids.erase(book.bids.begin());
            }
        }
    }
}

// ─── can_fill_completely ──────────────────────────────────────────────────────
bool MatchingEngine::can_fill_completely(const std::shared_ptr<Order>& order, const OrderBook& book) const {
    uint32_t needed = order->remaining();
    if (needed == 0) return true;

    if (order->side == Side::BUY) {
        for (auto it = book.asks.begin(); it != book.asks.end(); ++it) {
            if (order->type == OrderType::LIMIT && it->first > order->price) break;
            needed -= std::min(needed, static_cast<uint32_t>(it->second.total_qty));
            if (needed == 0) return true;
        }
    } else {
        for (auto it = book.bids.begin(); it != book.bids.end(); ++it) {
            if (order->type == OrderType::LIMIT && it->first < order->price) break;
            needed -= std::min(needed, static_cast<uint32_t>(it->second.total_qty));
            if (needed == 0) return true;
        }
    }
    return needed == 0;
}

// ─── execute_fill ─────────────────────────────────────────────────────────────
//
// The atomic "fill" operation. Given:
//   aggressor: the incoming order (market taker)
//   resting:   the passive order sitting in the book
//   fill_price: always resting's price
//
// Steps:
//   1. Compute fill quantity = min(aggressor.remaining, resting.remaining)
//   2. Update both orders' filled_qty and status
//   3. Update the resting order's PriceLevel total_qty
//   4. Remove the resting order from the book if fully filled
//   5. Generate TWO ExecutionReports — one for each party
//
void MatchingEngine::execute_fill(
    std::shared_ptr<Order>& aggressor,
    std::shared_ptr<Order>& resting,
    double fill_price,
    OrderBook& book)
{
    uint32_t fill_qty = std::min(aggressor->remaining(), resting->remaining());
    uint64_t ts       = now_ns();
    uint64_t exec_id  = next_exec_id++;

    // ── Update aggressor ─────────────────────────────────────────────────────
    aggressor->filled_qty += fill_qty;
    aggressor->status = (aggressor->remaining() == 0)
                        ? OrderStatus::FILLED
                        : OrderStatus::PARTIALLY_FILLED;

    // ── Update resting order ──────────────────────────────────────────────────
    resting->filled_qty += fill_qty;
    resting->status = (resting->remaining() == 0)
                      ? OrderStatus::FILLED
                      : OrderStatus::PARTIALLY_FILLED;

    // ── Update price level's running total ────────────────────────────────────
    // We need to find the level and decrement its total_qty.
    // The level is the one at fill_price on the resting side.
    if (resting->side == Side::SELL) {
        auto it = book.asks.find(fill_price);
        if (it != book.asks.end()) {
            it->second.update_qty(-static_cast<int64_t>(fill_qty));
        }
    } else {
        auto it = book.bids.find(fill_price);
        if (it != book.bids.end()) {
            it->second.update_qty(-static_cast<int64_t>(fill_qty));
        }
    }

    // ── Remove fully-filled resting order from level ──────────────────────────
    if (resting->remaining() == 0) {
        // Pop from front of the deque (it IS the front — we always start there)
        // Also remove from the secondary index
        if (resting->side == Side::SELL) {
            auto it = book.asks.find(fill_price);
            if (it != book.asks.end() && !it->second.orders.empty()) {
                it->second.orders.pop_front();
            }
        } else {
            auto it = book.bids.find(fill_price);
            if (it != book.bids.end() && !it->second.orders.empty()) {
                it->second.orders.pop_front();
            }
        }
        book.order_index.erase(resting->order_id);
    }

    // ── Generate ExecutionReports ─────────────────────────────────────────────
    // Report for the resting (passive) party
    ExecutionReport resting_report;
    resting_report.exec_id             = exec_id;
    resting_report.order_id            = resting->order_id;
    resting_report.aggressor_order_id  = aggressor->order_id;
    resting_report.trader_id           = resting->trader_id;
    resting_report.timestamp_ns        = ts;
    resting_report.exec_price          = fill_price;
    resting_report.exec_qty            = fill_qty;
    resting_report.side                = resting->side;
    std::memcpy(resting_report.symbol, resting->symbol, 8);

    // Report for the aggressor (active) party
    ExecutionReport aggressor_report;
    aggressor_report.exec_id            = exec_id;
    aggressor_report.order_id           = aggressor->order_id;
    aggressor_report.aggressor_order_id = aggressor->order_id;
    aggressor_report.trader_id          = aggressor->trader_id;
    aggressor_report.timestamp_ns       = ts;
    aggressor_report.exec_price         = fill_price;
    aggressor_report.exec_qty           = fill_qty;
    aggressor_report.side               = aggressor->side;
    std::memcpy(aggressor_report.symbol, aggressor->symbol, 8);

    fills_generated++;
    volume_traded += fill_qty;
    total_value_traded += (fill_qty * fill_price);

    // Notify subscribers (market maker, traders, risk system, etc.)
    on_fill(resting_report);
    on_fill(aggressor_report);
}
