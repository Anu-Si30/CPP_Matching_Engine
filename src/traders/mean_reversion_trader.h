#pragma once
// =============================================================================
// mean_reversion_trader.h  —  Bets that price returns to its rolling average
//
// Strategy: compute a rolling z-score of the mid price:
//
//   z = (current_price - rolling_mean) / rolling_stddev
//
// Signal:
//   z > +threshold  →  price is TOO HIGH  → SELL (expect reversion down)
//   z < -threshold  →  price is TOO LOW   → BUY  (expect reversion up)
//
// Intuition: if AAPL normally trades at $150 and suddenly spikes to $151.50,
// a mean reversion trader thinks "this is a temporary deviation, it'll come
// back to $150" and sells. This is the opposite of momentum trading.
//
// The two strategies (momentum vs mean reversion) are natural counterparties.
// In real markets, some regimes favor momentum (trending markets) and others
// favor mean reversion (range-bound markets). Having both in the simulation
// creates realistic competition.
//
// Parameters:
//   window      = 30   ticks (rolling window for mean/stddev)
//   entry_z     = 1.5         (z-score threshold to enter a trade)
//   exit_z      = 0.5         (z-score threshold to exit/flatten)
//   trade_qty   = 25          shares per signal
//   max_pos     = 200         max absolute position before stopping
// =============================================================================

#include "traders/trading_agent.h"
#include <deque>
#include <cmath>
#include <cstring>

class MeanReversionTrader : public TradingAgent {
public:
    struct Config {
        int    window    = 30;
        double entry_z   = 1.5;
        double exit_z    = 0.5;
        int    trade_qty = 25;
        int    max_pos   = 200;
        Config() : window(30), entry_z(1.5), exit_z(0.5), trade_qty(25), max_pos(200) {}
    };

    explicit MeanReversionTrader(uint64_t id, Config cfg = Config()) : cfg_(cfg) {
        trader_id = id;
        name      = "MeanRevTrader";
    }

    std::vector<std::shared_ptr<Order>>
    on_tick(const OrderBook& book, uint64_t ts) override {
        double mid = book.mid_price();
        if (mid <= 0.0) return {};

        price_history_.push_back(mid);
        if (static_cast<int>(price_history_.size()) > cfg_.window)
            price_history_.pop_front();

        if (static_cast<int>(price_history_.size()) < cfg_.window)
            return {};   // Warm-up period

        double mean   = compute_mean();
        double stddev = compute_stddev(mean);
        if (stddev < 1e-6) return {};   // Flat market — no signal

        double z = (mid - mean) / stddev;
        last_z_  = z;

        std::vector<std::shared_ptr<Order>> actions;

        // ── Entry logic ────────────────────────────────────────────────────
        if (z > cfg_.entry_z && inventory() > -cfg_.max_pos) {
            // Price too high → sell, expect reversion
            actions.push_back(make_order(Side::SELL, ts,
                // Use aggressive limit just inside the spread for faster fill
                mid - 0.01));
        } else if (z < -cfg_.entry_z && inventory() < cfg_.max_pos) {
            // Price too low → buy, expect reversion
            actions.push_back(make_order(Side::BUY, ts,
                mid + 0.01));
        }

        // ── Exit logic: flatten when price reverts ────────────────────────
        // If we're long and z is now positive (price above mean), start exiting
        // If we're short and z is now negative (price below mean), start exiting
        else if (std::abs(z) < cfg_.exit_z && inventory() != 0) {
            Side exit_side = (inventory() > 0) ? Side::SELL : Side::BUY;
            uint32_t exit_qty = static_cast<uint32_t>(
                std::min(static_cast<int64_t>(cfg_.trade_qty),
                         std::abs(inventory())));
            if (exit_qty > 0) {
                auto o = global_order_pool.acquire();
                o->order_id     = next_order_id();
                o->trader_id    = trader_id;
                o->timestamp_ns = ts;
                o->side         = exit_side;
                o->type         = OrderType::MARKET;  // Exit aggressively
                o->price        = 0.0;
                o->quantity     = exit_qty;
                o->filled_qty   = 0;
                o->status       = OrderStatus::NEW;
                o->set_symbol(symbol_.c_str());
                actions.push_back(o);
            }
        }

        return actions;
    }

    void on_fill(const ExecutionReport& r) override {
        record_fill(r);
    }

    void set_symbol(const std::string& sym) { symbol_ = sym; }

    double last_z_score() const { return last_z_; }

private:
    Config             cfg_;
    std::deque<double> price_history_;
    std::string        symbol_ = "AAPL";
    double             last_z_ = 0.0;

    double compute_mean() const {
        double sum = 0.0;
        for (double p : price_history_) sum += p;
        return sum / price_history_.size();
    }

    double compute_stddev(double mean) const {
        double var = 0.0;
        for (double p : price_history_) var += (p - mean) * (p - mean);
        return std::sqrt(var / price_history_.size());
    }

    std::shared_ptr<Order> make_order(Side side, uint64_t ts, double price) {
        auto o = global_order_pool.acquire();
        o->order_id     = next_order_id();
        o->trader_id    = trader_id;
        o->timestamp_ns = ts;
        o->side         = side;
        o->type         = OrderType::LIMIT;
        o->price        = std::round(price * 100.0) / 100.0;
        o->quantity     = static_cast<uint32_t>(cfg_.trade_qty);
        o->filled_qty   = 0;
        o->status       = OrderStatus::NEW;
        o->set_symbol(symbol_.c_str());
        return o;
    }
};
