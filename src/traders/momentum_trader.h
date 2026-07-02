#pragma once
// =============================================================================
// momentum_trader.h  —  Trades with the trend using a moving average crossover
//
// Strategy: track two moving averages of the mid price:
//   - Short MA (fast): reacts quickly to recent price moves
//   - Long  MA (slow): captures the broader trend
//
// Signal:
//   fast MA > slow MA by more than threshold → BULLISH  → submit MARKET BUY
//   fast MA < slow MA by more than threshold → BEARISH  → submit MARKET SELL
//
// Why moving average crossover?
//   It's one of the oldest and most studied technical signals. When the
//   short-term average rises above the long-term average, it suggests
//   recent momentum is upward — buy before more people do.
//   This agent is "informed" in the sense that it follows price direction,
//   but it's also a "trend follower" which means it's often late.
//
// In market microstructure terms, momentum traders are "informed flow" —
// they impose adverse selection costs on market makers because they tend
// to trade in the direction prices are already moving.
//
// Parameters (configurable):
//   short_window = 5   ticks  (fast MA)
//   long_window  = 20  ticks  (slow MA)
//   threshold    = 0.001      (0.1% divergence required to signal)
//   trade_qty    = 50         shares per signal
// =============================================================================

#include "traders/trading_agent.h"
#include <deque>
#include <numeric>
#include <cstring>

class MomentumTrader : public TradingAgent {
public:
    struct Config {
        int    short_window = 5;
        int    long_window  = 20;
        double threshold    = 0.001;
        int    trade_qty    = 50;
        Config() : short_window(5), long_window(20), threshold(0.001), trade_qty(50) {}
    };

    explicit MomentumTrader(uint64_t id, Config cfg = Config()) : cfg_(cfg) {
        trader_id = id;
        name      = "MomentumTrader";
    }

    std::vector<std::shared_ptr<Order>>
    on_tick(const OrderBook& book, uint64_t ts) override {
        double mid = book.mid_price();
        if (mid <= 0.0) return {};

        // Add latest mid price to history, maintain rolling window
        price_history_.push_back(mid);
        if (static_cast<int>(price_history_.size()) > cfg_.long_window)
            price_history_.pop_front();

        // Not enough data yet to compute both MAs
        if (static_cast<int>(price_history_.size()) < cfg_.long_window)
            return {};

        double fast_ma = compute_ma(cfg_.short_window);
        double slow_ma = compute_ma(cfg_.long_window);

        // Signal: only trade when divergence exceeds threshold
        bool bullish = fast_ma > slow_ma * (1.0 + cfg_.threshold);
        bool bearish = fast_ma < slow_ma * (1.0 - cfg_.threshold);

        if (!bullish && !bearish) return {};  // No signal this tick

        // Don't flip-flop: only trade if direction has changed
        Side signal_side = bullish ? Side::BUY : Side::SELL;
        if (signal_side == last_signal_) return {};  // Already positioned this way
        last_signal_ = signal_side;

        auto order = global_order_pool.acquire();
        order->order_id     = next_order_id();
        order->trader_id    = trader_id;
        order->timestamp_ns = ts;
        order->side         = signal_side;
        order->type         = OrderType::MARKET;   // Momentum = aggressive taker
        order->price        = 0.0;
        order->quantity     = static_cast<uint32_t>(cfg_.trade_qty);
        order->filled_qty   = 0;
        order->status       = OrderStatus::NEW;
        order->set_symbol(symbol_.c_str());

        return { order };
    }

    void on_fill(const ExecutionReport& r) override {
        record_fill(r);
    }

    void set_symbol(const std::string& sym) { symbol_ = sym; }

    // Expose for diagnostics
    double fast_ma_last() const {
        if (static_cast<int>(price_history_.size()) < cfg_.short_window) return 0;
        return compute_ma(cfg_.short_window);
    }
    double slow_ma_last() const {
        if (static_cast<int>(price_history_.size()) < cfg_.long_window) return 0;
        return compute_ma(cfg_.long_window);
    }

private:
    Config           cfg_;
    std::deque<double> price_history_;
    std::string      symbol_ = "AAPL";
    Side             last_signal_ = Side::BUY;  // Tracks last direction to avoid repeats
    bool             has_signal_  = false;

    double compute_ma(int window) const {
        // Average of the last `window` prices in the deque
        double sum = 0.0;
        int    n   = static_cast<int>(price_history_.size());
        int    start = n - window;
        for (int i = start; i < n; i++)
            sum += price_history_[i];
        return sum / window;
    }
};
