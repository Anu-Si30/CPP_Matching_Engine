#pragma once
// =============================================================================
// trading_agent.h  —  Abstract base class for all trading agents
//
// Every agent in the simulation — market maker, random trader, momentum
// trader — inherits from this and implements two methods:
//
//   on_tick()  : called each simulation step. Returns orders to submit.
//   on_fill()  : called when one of this agent's orders gets filled.
//
// Why a base class?
//   The simulation loop can hold a vector<TradingAgent*> and call on_tick()
//   on every agent without knowing what type they are. Adding a new agent
//   type requires zero changes to the simulation loop.
//
// Why return orders instead of submitting directly?
//   Agents don't hold a reference to the Exchange. They are pure decision
//   logic. The simulation loop submits their orders. This makes agents
//   independently testable and keeps concerns separated.
// =============================================================================

#include "orderbook/types.h"
#include "orderbook/order_book.h"
#include <vector>
#include <memory>
#include <string>
#include <cstdint>
#include <cmath>

class TradingAgent {
public:
    uint64_t    trader_id;
    std::string name;

    virtual ~TradingAgent() = default;

    // ── Called every simulation tick ──────────────────────────────────────────
    // book: read-only snapshot of the current order book for this agent's symbol
    // ts  : current simulation timestamp in nanoseconds
    // Returns a list of orders to submit (can be empty if no action this tick)
    virtual std::vector<std::shared_ptr<Order>>
    on_tick(const OrderBook& book, uint64_t ts) = 0;

    // ── Called when one of this agent's orders gets a fill ───────────────────
    // Agents use this to update their PnL, inventory, and internal state.
    virtual void on_fill(const ExecutionReport& report) = 0;

    // ── Optional: called when one of this agent's orders is cancelled ─────────
    virtual void on_cancel(uint64_t /*order_id*/) {}

    // ── PnL and inventory tracking ────────────────────────────────────────────
    // These are updated in on_fill() by derived classes.
    // Positive inventory = long (holding shares).
    // Negative inventory = short (owe shares).
    int64_t  inventory()     const { return inventory_; }
    double   realized_pnl()  const { return realized_pnl_; }
    uint64_t fill_count()    const { return fill_count_; }
    uint64_t order_count()   const { return order_count_; }

    // Unrealized PnL: how much we'd make/lose if we closed right now at mid
    double unrealized_pnl(double current_mid) const {
        if (inventory_ == 0 || avg_cost_ == 0.0) return 0.0;
        return inventory_ * (current_mid - avg_cost_);
    }

    double total_pnl(double current_mid) const {
        return realized_pnl_ + unrealized_pnl(current_mid);
    }

    double max_drawdown() const { return max_drawdown_; }
    double sharpe_ratio() const {
        if (pnl_returns_.size() < 2) return 0.0;
        double sum = 0.0, sq_sum = 0.0;
        for (double r : pnl_returns_) {
            sum += r;
            sq_sum += r * r;
        }
        double mean = sum / pnl_returns_.size();
        double variance = (sq_sum / pnl_returns_.size()) - (mean * mean);
        if (variance <= 0.0) return 0.0;
        // Annualize it assuming high-frequency (just a simple scaler for simulation)
        return (mean / std::sqrt(variance)); 
    }
    double avg_spread_captured() const {
        if (closing_trades_ == 0) return 0.0;
        return realized_pnl_ / closing_trades_;
    }

    // Called every tick to sample PnL for Sharpe
    void record_pnl_sample(double current_mid) {
        double current_pnl = total_pnl(current_mid);
        double step_return = current_pnl - last_pnl_sample_;
        pnl_returns_.push_back(step_return);
        last_pnl_sample_ = current_pnl;

        if (current_pnl > peak_pnl_) peak_pnl_ = current_pnl;
        double drawdown = peak_pnl_ - current_pnl;
        if (drawdown > max_drawdown_) max_drawdown_ = drawdown;
    }

protected:
    // ── Shared PnL accounting ─────────────────────────────────────────────────
    // Derived classes call this from on_fill() to keep accounting consistent.
    void record_fill(const ExecutionReport& r) {
        fill_count_++;

        if (r.side == Side::BUY) {
            // Bought: inventory up, average cost updated
            double old_cost  = avg_cost_ * (inventory_ > 0 ? inventory_ : 0);
            inventory_      += static_cast<int64_t>(r.exec_qty);
            if (inventory_ > 0)
                avg_cost_ = (old_cost + r.exec_price * r.exec_qty) / inventory_;
        } else {
            // Sold: if closing a long, realize PnL
            if (inventory_ > 0) {
                int64_t closing = std::min(static_cast<int64_t>(r.exec_qty), inventory_);
                realized_pnl_  += closing * (r.exec_price - avg_cost_);
                closing_trades_ += closing;
            }
            inventory_ -= static_cast<int64_t>(r.exec_qty);
        }
    }

    // ── Order ID generation ───────────────────────────────────────────────────
    // Encodes trader_id in the upper 32 bits so order_ids are globally unique
    // and attributable without a lookup table.
    uint64_t next_order_id() {
        order_count_++;
        return (trader_id << 32) | ++id_counter_;
    }

private:
    int64_t  inventory_    = 0;
    double   realized_pnl_ = 0.0;
    double   avg_cost_     = 0.0;
    uint64_t fill_count_   = 0;
    uint64_t order_count_  = 0;
    uint64_t id_counter_   = 0;

    // Analytics
    double   peak_pnl_        = 0.0;
    double   max_drawdown_    = 0.0;
    double   last_pnl_sample_ = 0.0;
    uint64_t closing_trades_  = 0;
    std::vector<double> pnl_returns_;
};
