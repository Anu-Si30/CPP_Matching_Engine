#pragma once
// =============================================================================
// types.h  —  The fundamental data types of the exchange
//
// Every order, fill, and status in the entire system flows through these types.
// Design goal: fit the hot-path Order struct in ONE cache line (64 bytes).
// =============================================================================

#include <cstdint>
#include <cstring>
#include <chrono>

// ─── Helper: current wall-clock time in nanoseconds ──────────────────────────
inline uint64_t now_ns() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<nanoseconds>(
            high_resolution_clock::now().time_since_epoch()
        ).count()
    );
}

// ─── Enumerations ─────────────────────────────────────────────────────────────

// Which side of the book does this order live on?
enum class Side : uint8_t {
    BUY  = 0,   // Bid side — willing to buy at price or lower
    SELL = 1    // Ask side — willing to sell at price or higher
};

// What kind of order is it?
enum class OrderType : uint8_t {
    LIMIT  = 0,  // Rest in the book at a specific price until matched or cancelled
    MARKET = 1   // Execute immediately at the best available price (no price posted)
};

// Lifecycle state of an order
enum class OrderStatus : uint8_t {
    NEW              = 0,  // Just submitted, not yet processed
    PARTIALLY_FILLED = 1,  // Some quantity has been matched
    FILLED           = 2,  // Completely matched — order is done
    CANCELLED        = 3,  // Cancelled by the submitter before full fill
    REJECTED         = 4   // Rejected by the exchange (e.g., invalid price)
};

// ─── Order ────────────────────────────────────────────────────────────────────
//
// alignas(64): forces this struct to start at a 64-byte boundary.
// This guarantees the entire struct fits within ONE CPU cache line.
// On a modern CPU, reading across a cache-line boundary requires TWO memory
// fetches instead of one — a 2x penalty on your hottest data structure.
//
// Field layout is deliberate: doubles first (8-byte aligned), then uint32s,
// then the char array, then the 1-byte enums, then explicit padding.
//
struct alignas(64) Order {
    uint64_t    order_id;        // Globally unique ID for this order
    uint64_t    trader_id;       // ID of the agent/trader who submitted it
    uint64_t    timestamp_ns;    // Nanosecond timestamp — used for FIFO tiebreaking

    double      price;           // Limit price (0.0 for MARKET orders)
    uint32_t    quantity;        // Total quantity requested
    uint32_t    filled_qty;      // How much has been matched so far

    char        symbol[8];       // Instrument ticker, null-padded: "AAPL\0\0\0\0"

    Side        side;            // BUY or SELL
    OrderType   type;            // LIMIT or MARKET
    OrderStatus status;          // Current lifecycle state

    uint8_t     _pad[5];         // Explicit padding to reach exactly 64 bytes

    // ── Convenience methods ──────────────────────────────────────────────────

    // How many shares are still unfilled?
    uint32_t remaining() const noexcept {
        return quantity - filled_qty;
    }

    // Is this order still eligible to be matched?
    bool is_active() const noexcept {
        return status == OrderStatus::NEW ||
               status == OrderStatus::PARTIALLY_FILLED;
    }

    // Set the symbol from a C-string safely (always null-terminates within 8 bytes)
    void set_symbol(const char* sym) {
        std::memset(symbol, 0, 8);
        std::strncpy(symbol, sym, 7);
    }
};

// Verify at compile time that Order is exactly 64 bytes.
// If this fails, something is misaligned — fix the padding.
static_assert(sizeof(Order) == 64, "Order must be exactly 64 bytes (one cache line)");

// ─── ExecutionReport ─────────────────────────────────────────────────────────
//
// Generated every time a fill (match) occurs.
// Both the resting order and the aggressor (incoming) order receive one.
// This is what real exchanges send back to traders as "execution reports."
//
struct ExecutionReport {
    uint64_t exec_id;              // Unique ID for this fill event
    uint64_t order_id;             // The order that was filled (could be resting or aggressor)
    uint64_t aggressor_order_id;   // The order that initiated the match
    uint64_t trader_id;            // Trader who owns order_id
    uint64_t timestamp_ns;         // When the fill happened

    double   exec_price;           // The price at which the fill occurred
    uint32_t exec_qty;             // How many shares were filled in this event

    Side     side;                 // BUY or SELL (from the perspective of order_id)
    char     symbol[8];            // Which instrument
};
