#pragma once
// =============================================================================
// random_trader.h  —  Simulates uninformed retail-like order flow
//
// The random trader submits orders at random prices around the mid price.
// It has no edge — it doesn't use any information about where the price
// is going. Its purpose in the simulation is to:
//
//   1. Provide background liquidity noise (realistic market activity)
//   2. Generate fills that move money between agents
//   3. Give the market maker someone to trade against
//
// In academic market microstructure, this is called "uninformed flow" or
// "noise trading." Real markets have a lot of it — retail investors clicking
// "Buy" on their phones without any informational edge.
//
// The random trader posts:
//   - 70% LIMIT orders at a random price near mid (provides liquidity)
//   - 30% MARKET orders (takes liquidity, generates immediate fills)
//   - 50/50 buy/sell split (no directional bias)
//   - Random quantity 1-100 shares
// =============================================================================

#include "traders/trading_agent.h"
#include <random>
#include <cmath>
#include <cstring>

class RandomTrader : public TradingAgent {
public:
    explicit RandomTrader(uint64_t id, uint64_t seed = 42) {
        trader_id = id;
        name      = "RandomTrader";
        rng_.seed(seed);
    }

    std::vector<std::shared_ptr<Order>>
    on_tick(const OrderBook& book, uint64_t ts) override {
        double mid = book.mid_price();
        if (mid <= 0.0) return {};  // No price reference yet, sit out

        std::uniform_real_distribution<double> price_noise(-0.20, 0.20);
        std::uniform_int_distribution<int>     qty_dist(1, 100);
        std::uniform_real_distribution<double> prob(0.0, 1.0);

        auto order = global_order_pool.acquire();
        order->order_id     = next_order_id();
        order->trader_id    = trader_id;
        order->timestamp_ns = ts;
        order->filled_qty   = 0;
        order->status       = OrderStatus::NEW;
        order->quantity     = static_cast<uint32_t>(qty_dist(rng_));
        order->side         = (prob(rng_) < 0.5) ? Side::BUY : Side::SELL;
        order->set_symbol(symbol_.c_str());

        if (prob(rng_) < 0.30) {
            // Market order: immediate fill at best available price
            order->type  = OrderType::MARKET;
            order->price = 0.0;
        } else {
            // Limit order: random price within ±$0.20 of mid, rounded to cents
            order->type  = OrderType::LIMIT;
            double raw   = mid + price_noise(rng_);
            order->price = std::round(raw * 100.0) / 100.0;
            if (order->price <= 0.0) order->price = 0.01;
        }

        return { order };
    }

    void on_fill(const ExecutionReport& r) override {
        record_fill(r);
    }

    void set_symbol(const std::string& sym) { symbol_ = sym; }

private:
    std::mt19937_64 rng_;
    std::string     symbol_ = "AAPL";
};
