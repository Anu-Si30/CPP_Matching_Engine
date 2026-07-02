#pragma once
// =============================================================================
// market_maker.h  —  Provides two-sided liquidity and manages inventory
//
// The Market Maker is the core participant in modern electronic markets.
// Its goal is to earn the bid-ask spread by constantly quoting on both
// sides of the book (providing liquidity). 
//
// Key challenges for a Market Maker:
//   1. Adverse Selection: If the price moves against them (e.g. they get
//      run over by a momentum trader), they lose money.
//   2. Inventory Risk: If they accumulate too many shares (long or short),
//      they are exposed to price movements.
//
// This agent solves inventory risk by "skewing" its quotes.
//   - If it becomes too long (e.g. +300 shares), it lowers its prices.
//     Lower bid = harder to buy more. Lower ask = easier to sell and flatten.
//
// Interaction with Exchange:
//   To update quotes continuously, the MM needs to cancel its old orders.
//   We pass a pointer to the Exchange so it can issue cancels during on_tick().
//
// Parameters:
//   half_spread    = 0.05    (Quotes $0.05 above and below fair value)
//   quote_qty      = 100     (Shares per quote)
//   max_position   = 500     (Stops quoting on one side if limit reached)
//   skew_factor    = 0.01    (Shifts quotes 1 cent per 100 shares of inventory)
// =============================================================================

#include "traders/trading_agent.h"
#include "exchange/exchange.h"
#include <cmath>

class MarketMaker : public TradingAgent {
public:
    struct Config {
        double half_spread    = 0.05;
        int    quote_qty      = 100;
        int    max_position   = 500;
        double skew_factor    = 0.01;
        
        Config() : half_spread(0.05), quote_qty(100), max_position(500), skew_factor(0.01) {}
    };

    explicit MarketMaker(uint64_t id, Exchange* ex, Config cfg = Config()) 
        : ex_(ex), cfg_(cfg) {
        trader_id = id;
        name      = "MarketMaker";
    }

    std::vector<std::shared_ptr<Order>>
    on_tick(const OrderBook& book, uint64_t ts) override {
        double mid = book.mid_price();
        if (mid <= 0.0) return {}; // Wait until there is a valid mid price
        
        record_pnl_sample(mid);

        // 1. Cancel previous quotes to reposition them
        if (active_bid_id_ != 0) {
            ex_->cancel_order(active_bid_id_);
            active_bid_id_ = 0;
        }
        if (active_ask_id_ != 0) {
            ex_->cancel_order(active_ask_id_);
            active_ask_id_ = 0;
        }

        // 2. Compute inventory skew
        // Long inventory (>0) -> negative skew -> lowers bid/ask
        // Short inventory (<0) -> positive skew -> raises bid/ask
        double skew = -(static_cast<double>(inventory()) / 100.0) * cfg_.skew_factor;
        
        double bid_price = mid - cfg_.half_spread + skew;
        double ask_price = mid + cfg_.half_spread + skew;
        
        // Round to nearest cent
        bid_price = std::round(bid_price * 100.0) / 100.0;
        ask_price = std::round(ask_price * 100.0) / 100.0;

        // Ensure quotes don't cross (can happen if skew is extreme)
        if (bid_price >= ask_price) {
            bid_price = mid - 0.01;
            ask_price = mid + 0.01;
        }
        if (bid_price <= 0.0) bid_price = 0.01;

        std::vector<std::shared_ptr<Order>> new_quotes;

        // 3. Post new BID if not at max long position
        if (inventory() < cfg_.max_position) {
            auto bid = global_order_pool.acquire();
            bid->order_id     = next_order_id();
            bid->trader_id    = trader_id;
            bid->timestamp_ns = ts;
            bid->side         = Side::BUY;
            bid->type         = OrderType::LIMIT;
            bid->price        = bid_price;
            bid->quantity     = cfg_.quote_qty;
            bid->filled_qty   = 0;
            bid->status       = OrderStatus::NEW;
            bid->set_symbol(symbol_.c_str());
            active_bid_id_    = bid->order_id;
            new_quotes.push_back(bid);
        }

        // 4. Post new ASK if not at max short position
        if (inventory() > -cfg_.max_position) {
            auto ask = global_order_pool.acquire();
            ask->order_id     = next_order_id();
            ask->trader_id    = trader_id;
            ask->timestamp_ns = ts;
            ask->side         = Side::SELL;
            ask->type         = OrderType::LIMIT;
            ask->price        = ask_price;
            ask->quantity     = cfg_.quote_qty;
            ask->filled_qty   = 0;
            ask->status       = OrderStatus::NEW;
            ask->set_symbol(symbol_.c_str());
            active_ask_id_    = ask->order_id;
            new_quotes.push_back(ask);
        }

        return new_quotes;
    }

    void on_fill(const ExecutionReport& r) override {
        record_fill(r);
        // We don't bother clearing active_bid_id_ if it was fully filled,
        // because cancel_order gracefully ignores non-existent orders.
    }

    void set_symbol(const std::string& sym) { symbol_ = sym; }

private:
    Exchange* ex_;
    Config    cfg_;
    std::string symbol_ = "AAPL";
    
    uint64_t active_bid_id_ = 0;
    uint64_t active_ask_id_ = 0;
};
