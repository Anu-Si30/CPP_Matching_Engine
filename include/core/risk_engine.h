#pragma once
// =============================================================================
// risk_engine.h  —  Pre-trade risk checks and exposure tracking
//
// In a real electronic trading firm, the Risk Engine sits between the traders
// and the Exchange. It validates every single order before it is sent to the
// market. If an algo goes rogue or a trader makes a fat-finger mistake,
// the Risk Engine rejects it to prevent catastrophic losses.
//
// This engine tracks:
//   1. Realized position (shares currently held)
//   2. Open exposure (shares sitting in the order book, waiting to be filled)
//
// Why track open exposure?
//   If you are allowed to hold 1,000 shares max, and you submit a BUY order
//   for 1,000 shares, that order rests in the book. If you try to submit 
//   ANOTHER BUY for 500 shares, the risk engine MUST reject it! Even though 
//   your current position is 0, if both orders fill, you will hold 1,500 shares,
//   exceeding your limit.
//
// Operations:
//   - check_order() : Intercepts an order before it goes to the Exchange.
//   - on_fill()     : Converts open exposure into realized position.
//   - on_cancel()   : Removes open exposure when an order is cancelled.
// =============================================================================

#include "core/types.h"
#include <unordered_map>
#include <string>
#include <cstring>
#include <vector>

struct RiskLimits {
    int64_t max_long_position  = 1000;
    int64_t max_short_position = 1000;
};

struct TraderRiskState {
    int64_t position      = 0;   // Net filled shares
    int64_t open_buy_qty  = 0;   // Resting buy order shares
    int64_t open_sell_qty = 0;   // Resting sell order shares
    
    RiskLimits limits;
};

class RiskEngine {
public:
    void set_limits(uint64_t trader_id, const std::string& symbol, const RiskLimits& limits) {
        state_[trader_id][symbol].limits = limits;
    }

    // Pre-trade risk check.
    // Must be called BEFORE submitting to the Exchange.
    // Returns true if approved, false if rejected.
    bool check_order(const std::shared_ptr<Order>& order) {
        char sym_buf[9] = {};
        strncpy(sym_buf, order->symbol, 8);
        std::string sym(sym_buf);
        auto& tr_state = state_[order->trader_id][sym];
        
        if (order->side == Side::BUY) {
            // Potential max long = what we already hold + what is waiting to buy + this new order
            int64_t potential_long = tr_state.position + tr_state.open_buy_qty + order->quantity;
            
            if (potential_long > tr_state.limits.max_long_position) {
                order->status = OrderStatus::REJECTED;
                return false;
            }
            
            // Approved -> book this as open exposure
            tr_state.open_buy_qty += order->quantity;
            order_exposures_[order->order_id] = { order->quantity, Side::BUY, order->trader_id, sym };
            return true;
            
        } else {
            // Potential max short = what we hold (could be negative) - what is waiting to sell - this new order
            // Note: short positions are represented as negative numbers, so we compare against -limit
            int64_t potential_short = tr_state.position - tr_state.open_sell_qty - order->quantity;
            
            if (potential_short < -tr_state.limits.max_short_position) {
                order->status = OrderStatus::REJECTED;
                return false;
            }
            
            // Approved -> book this as open exposure
            tr_state.open_sell_qty += order->quantity;
            order_exposures_[order->order_id] = { order->quantity, Side::SELL, order->trader_id, sym };
            return true;
        }
    }

    // Must be called on every fill so the Risk Engine can move shares
    // from "open exposure" to "realized position".
    void on_fill(const ExecutionReport& r) {
        auto it = order_exposures_.find(r.order_id);
        if (it != order_exposures_.end()) {
            char sym_buf[9] = {};
            strncpy(sym_buf, r.symbol, 8);
            std::string sym(sym_buf);
            auto& tr_state = state_[r.trader_id][sym];
            
            if (r.side == Side::BUY) {
                tr_state.open_buy_qty -= r.exec_qty;
                tr_state.position += r.exec_qty;
            } else {
                tr_state.open_sell_qty -= r.exec_qty;
                tr_state.position -= r.exec_qty;
            }
            
            it->second.qty -= r.exec_qty;
            if (it->second.qty <= 0) {
                order_exposures_.erase(it);
            }
        }
    }

    // Must be called when an order is cancelled so open exposure is freed up.
    void on_cancel(uint64_t order_id) {
        auto it = order_exposures_.find(order_id);
        if (it != order_exposures_.end()) {
            auto& tr_state = state_[it->second.trader_id][it->second.symbol];
            if (it->second.side == Side::BUY) {
                tr_state.open_buy_qty -= it->second.qty;
            } else {
                tr_state.open_sell_qty -= it->second.qty;
            }
            order_exposures_.erase(it);
        }
    }

    // Introspection
    const TraderRiskState& get_state(uint64_t trader_id, const std::string& symbol) {
        return state_[trader_id][symbol];
    }

private:
    // trader_id -> symbol -> RiskState
    std::unordered_map<uint64_t, std::unordered_map<std::string, TraderRiskState>> state_;
    
    struct OrderExposure {
        uint32_t qty;
        Side side;
        uint64_t trader_id;
        std::string symbol;
    };
    std::unordered_map<uint64_t, OrderExposure> order_exposures_;
};
