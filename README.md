# C++ High-Performance Matching Engine

A from-scratch implementation of an electronic exchange matching engine in C++, built to demonstrate the systems engineering, data structures, and market microstructure knowledge that HFT firms care about.

> **Status:** Stage 2 complete — Exchange orchestrator, order modification, multiple symbols.

---

## What This Project Is

Real exchanges like NASDAQ and NYSE are, at their core, software systems that:

1. Accept orders from traders ("buy 100 shares of AAPL at $150")
2. Store unfilled orders in a data structure called an **order book**
3. Continuously check whether any two orders should trade against each other
4. When they do, generate a **fill** and notify both parties

This project builds that system from the ground up in C++, then layers trading agents, a market maker, risk management, and a live terminal visualization on top of it.

The architecture mirrors how firms like Citadel and Virtu actually think about these systems — the exchange is the platform, and the market maker is an application that runs on it.

---

## Architecture

```
                 Historical Data (ITCH / CSV)
                          │
                          ▼
              ┌────────────────────────┐
              │      Exchange Core     │
              │  ┌──────────────────┐  │
              │  │   Order Book     │  │  ← Stage 1
              │  │  (per symbol)    │  │
              │  └────────┬─────────┘  │
              │           │            │
              │  ┌────────▼─────────┐  │
              │  │ Matching Engine  │  │  ← Stage 1
              │  │ Price-Time FIFO  │  │
              │  └────────┬─────────┘  │
              │           │            │
              │  ┌────────▼─────────┐  │
              │  │Execution Reports │  │  ← Stage 1
              │  └──────────────────┘  │
              └───────────┬────────────┘
                          │
          ┌───────────────┼──────────────┐
          │               │              │
          ▼               ▼              ▼
     Market Maker    Random Trader  Momentum Trader   ← Stages 3–4
          │
          ▼
   Risk / Inventory Engine                            ← Stage 5
          │
          ▼
   Terminal UI + Benchmarks                           ← Stages 7–8
```

---

## Stage 1 — Core Exchange Infrastructure

### What Stage 1 Builds

Stage 1 implements the exchange itself — the part that makes it a real exchange. Every other component in the project depends on this foundation.

Three components are implemented:

| Component | File(s) | Responsibility |
|-----------|---------|----------------|
| Order type system | `include/core/types.h` | Defines what an order is |
| Order Book | `include/core/order_book.h/.cpp` | Stores and organizes resting orders |
| Matching Engine | `include/core/matching_engine.h/.cpp` | Executes trades between orders |

---

### Component 1: The Order (`include/core/types.h`)

An order is a trader's instruction to the exchange: *"I want to buy/sell X shares of symbol Y at price Z."*

```cpp
struct alignas(64) Order {
    uint64_t    order_id;       // Unique ID — used for cancel/modify
    uint64_t    trader_id;      // Which agent submitted this
    uint64_t    timestamp_ns;   // Nanosecond arrival time — FIFO tiebreaker

    double      price;          // Limit price (0 for market orders)
    uint32_t    quantity;       // Total shares requested
    uint32_t    filled_qty;     // Shares matched so far

    char        symbol[8];      // Ticker: "AAPL\0\0\0\0"

    Side        side;           // BUY or SELL
    OrderType   type;           // LIMIT or MARKET
    OrderStatus status;         // NEW → PARTIALLY_FILLED → FILLED / CANCELLED
};
```

**Order types:**

- **LIMIT order** — "Buy at $99.90 *or better*." If no match exists, the order rests in the book and waits. This is how market makers operate — posting prices and waiting for someone to hit them.
- **MARKET order** — "Buy at *whatever price is available right now.*" Executes immediately against the best resting orders. No price posted, no resting. Used by traders who prioritize certainty of execution over price.

**`alignas(64)` — the cache line trick:**

The struct is forced to exactly 64 bytes — the size of one CPU cache line. When a CPU reads any data, it fetches the entire 64-byte cache line around it. If the struct crossed two cache lines, every order access would require two fetches. At 10 million orders per second, cutting that in half is a meaningful latency reduction.

The `static_assert(sizeof(Order) == 64)` enforces this at compile time — if a field is added or the layout changes, the build breaks immediately rather than silently regressing performance.

**Every fill generates an `ExecutionReport`:**

```cpp
struct ExecutionReport {
    uint64_t exec_id;             // Unique ID for this fill event
    uint64_t order_id;            // The order that was filled
    uint64_t aggressor_order_id;  // The incoming order that triggered the fill
    uint64_t trader_id;           // Owner of order_id
    uint64_t timestamp_ns;
    double   exec_price;          // Price at which the trade occurred
    uint32_t exec_qty;            // Shares traded in this event
    Side     side;
    char     symbol[8];
};
```

Two reports are generated per fill — one for each party. This is identical to how real exchanges notify traders.

---

### Component 2: The Order Book (`include/core/order_book.h/.cpp`)

The order book stores every resting (unfilled) limit order for one symbol. It has two sides:

```
ASK side — sellers, sorted lowest price first
  100.30 | 200 shares
  100.20 |  80 shares
  100.10 | 120 shares   ← best ask
  ─────────────────────── spread = $0.20
   99.90 | 100 shares   ← best bid
   99.80 | 200 shares
   99.70 | 150 shares
BID side — buyers, sorted highest price first
```

The **spread** is the gap between the best bid and best ask. When the spread closes to zero — a buyer's price meets a seller's price — a trade occurs.

#### Data Structure: `std::map` for price levels

```cpp
using BidMap = std::map<double, PriceLevel, std::greater<double>>;
using AskMap = std::map<double, PriceLevel, std::less<double>>;
```

`std::map` is a red-black tree (self-balancing BST). It provides:
- **O(log N)** insert and delete
- **O(1)** best-price access via `begin()` — the tree's edge node
- **Guaranteed ordering** for matching sweeps

`std::unordered_map` (hash map) was rejected because it has no ordering. You cannot find the best bid without scanning everything — that breaks the matching sweep.

The comparators ensure the best price is always at `begin()`:
- Bids use `std::greater<double>`: highest price = leftmost = `bids.begin()`
- Asks use `std::less<double>`: lowest price = leftmost = `asks.begin()`

#### Data Structure: `std::deque` within each price level

Multiple orders can exist at the same price. They form a **FIFO queue** — first submitted, first filled. This implements the time-priority half of price-time priority matching.

```cpp
struct PriceLevel {
    double   price;
    uint64_t total_qty;                            // Sum of all resting quantities
    std::deque<std::shared_ptr<Order>> orders;     // Front = oldest = highest priority
};
```

`std::deque` provides O(1) `push_back` (new orders join the back) and O(1) `pop_front` (filled orders leave the front). A `vector` would require O(N) shifting on every fill. A `list` has O(1) operations but requires a heap allocation per node and suffers pointer-chasing cache misses.

#### The Secondary Index — O(log N) cancellation

```cpp
std::unordered_map<uint64_t, OrderLocation> order_index;
// Maps: order_id → { side, price }
```

When a cancel arrives for `order_id = 7823`, the engine needs to find it. Without this index, it would scan every price level — O(N). With it:

1. `order_index[7823]` → `{SELL, 100.05}` — O(1) hash lookup
2. `asks[100.05]` → price level — O(log N) tree walk
3. Remove the order from the level's deque — O(k) where k = orders at that price (typically < 5)

Total: effectively O(log N). Empty price levels are immediately erased so `best_bid()` and `best_ask()` never return stale data.

---

### Component 3: The Matching Engine (`include/core/matching_engine.h/.cpp`)

The matching engine has one job: given an incoming order, determine what (if anything) it should trade against in the book.

#### Price-Time Priority (FIFO Matching)

Every major exchange uses this rule:

1. **Best price gets matched first.** An ask at $100.00 fills before one at $100.05.
2. **Among equal prices, the oldest order fills first.** If two sellers both offer $100.00, whoever submitted first gets the trade.

This rule incentivizes tighter prices (compete on price to get priority) and early submission (get in the queue early).

#### Fill Price Rule

**The fill always happens at the resting order's price, not the aggressor's.**

If an incoming BUY limit at $100.15 crosses a resting SELL at $100.10, the trade executes at $100.10. The buyer pays less than their maximum. The seller gets exactly what they asked. This rewards passive liquidity providers.

#### The Matching Algorithm

**For a LIMIT order:**

```
Incoming: BUY 60 shares @ limit $100.15

Check best ask = $100.10.
  $100.10 ≤ $100.15 → spread is crossed → match!

Fill qty = min(incoming.remaining=60, resting.remaining=100) = 60
Fill price = $100.10 (resting price)
Update: resting now has 40 shares left. Incoming is FULLY FILLED.

Check: is incoming fully filled? Yes → stop.
Any remaining quantity would be posted to the book.
```

**For a MARKET order:**

```
Incoming: MARKET BUY 175 shares

Level 1 (best ask = $100.00, qty=50):
  Fill 50 shares @ $100.00. Incoming has 125 left.

Level 2 (next ask = $100.05, qty=100):
  Fill 100 shares @ $100.05. Incoming has 25 left.

Level 3 (next ask = $100.10, qty=75):
  Fill 25 shares @ $100.10. Incoming is FULLY FILLED.
  75 - 25 = 50 shares remain in this level.
```

The average fill price (VWAP) was $100.0429 — higher than the $100.00 best ask. The difference ($0.0429/share) is **slippage** — the cost of demanding immediate execution.

#### The Fill Callback

The matching engine does not know about traders, risk limits, or the UI. It communicates only through a callback registered at construction:

```cpp
MatchingEngine engine([](const ExecutionReport& report) {
    // Called twice per fill — once for each party
    // Route to: trader PnL, risk system, UI display, etc.
});
```

This design decouples the matching logic from everything downstream. The engine stays stateless with respect to "who gets notified" — callers inject that behavior.

---

### What Stage 1 Proves

Running `stage1_test.exe` verifies six fundamental properties:

| Test | What it verifies |
|------|-----------------|
| **1. Book construction** | Orders that don't cross the spread rest silently — zero spurious fills |
| **2. Limit match** | BUY at $100.15 crosses SELL at $100.10 → fill at $100.10 (resting price) |
| **3. FIFO priority** | Two SELLs at same price — older one fills first, newer one untouched |
| **4. Market sweep** | MARKET BUY sweeps through 3 price levels, VWAP emerges naturally |
| **5. Cancellation** | Cancel removes the order, level survives if others remain at that price |
| **6. Partial fill** | Small buy partially fills large sell — sell stays in book with reduced qty |

---

### Build and Run

**Requirements:** GCC 6.3+ or any C++14-capable compiler. No external dependencies.

```bash
# Clone
git clone <repo-url>
cd CPP_Matching_Engine

# Build (MinGW on Windows)
mkdir build
g++ -std=c++14 -g -Wall -Iinclude \
    src/core/order_book.cpp \
    src/core/matching_engine.cpp \
    tests/stage1_test.cpp \
    -o build/stage1_test.exe

# Run
./build/stage1_test.exe
```

**Expected output:**
```
Order struct size: 64 bytes  (expected: 64)

=== TEST 1: Build an Order Book (no matches) ====
...
PASS

=== TEST 2: Limit Order Match (spread crossing) ==
...
PASS

=== TEST 3: Price-Time (FIFO) Priority ===========
...
PASS -- FIFO is working

=== TEST 4: Market Order Sweeps Multiple Levels ==
...
PASS -- Market order swept 175 shares across 3 levels

=== TEST 5: Order Cancellation ==================
...
PASS

=== TEST 6: Partial Fill ========================
...
PASS
```

---

### Project File Structure (Stage 1)

```
CPP_Matching_Engine/
├── CMakeLists.txt
├── README.md
├── include/
│   └── core/
│       ├── types.h              ← Order, ExecutionReport, enums
│       ├── order_book.h         ← PriceLevel, OrderBook
│       ├── matching_engine.h   ← MatchingEngine, FillCallback
│       └── exchange.h          ← Exchange orchestrator (Stage 2)
└── src/
│   └── core/
│       ├── order_book.cpp       ← add, cancel, depth snapshot
│       ├── matching_engine.cpp ← submit, match, execute_fill
│       └── exchange.cpp        ← symbol routing, modify, stats (Stage 2)
└── tests/
    ├── stage1_test.cpp          ← 6 Stage 1 verification tests
    └── stage2_test.cpp          ← 6 Stage 2 verification tests
```

---

## Roadmap

| Stage | Status | Description |
|-------|--------|-------------|
| **1** | ✅ **Complete** | Order book, matching engine, FIFO matching, cancellation, partial fills |
| **2** | ✅ **Complete** | Order modification, multiple symbols, Exchange orchestrator, global stats |
| 3 | 🔲 Next | Trading agents — random, momentum, mean reversion |
| 4 | 🔲 | Market maker — inventory-aware quoting, spread control, PnL |
| 5 | 🔲 | Risk / inventory engine — pre-trade checks, position limits |
| 6 | 🔲 | Multithreading — lock-free SPSC queue, thread-per-agent |
| 7 | 🔲 | Benchmarking harness — orders/sec, latency percentiles |
| 8 | 🔲 | Terminal UI — live order book display, market maker stats |
| 9 | 🔲 | Historical replay — NASDAQ ITCH 5.0 binary protocol parser |

---

## Key Design Decisions

| Decision | Rationale |
|----------|----------|
| `alignas(64)` on `Order` | One cache line per order — halves memory fetches on the hot path |
| `std::map` for bid/ask levels | Ordered iteration required for matching sweeps; `unordered_map` breaks this |
| `std::deque` per price level | O(1) push_back + O(1) pop_front — FIFO without vector's O(N) shifting |
| Secondary `order_index` (per book) | Makes cancel O(log N) instead of O(N) linear scan across all levels |
| Global `order_index` (Exchange) | Routes cancel/modify by order_id alone — callers don't need to know the symbol |
| Fill at resting price | Universal exchange rule — rewards passive liquidity providers |
| Callback-based fills | Decouples engine from downstream consumers; stays testable in isolation |
| Modify = cancel+reinsert (price/qty↑) | Prevents queue-position gaming; mirrors real exchange rules |
| Modify = in-place (qty↓) | Rewards risk reduction — the one modification without queue penalty |
| `static_assert(sizeof(Order)==64)` | Compile-time enforcement — layout regression breaks the build, not production |

---

## Concepts Demonstrated

**Data Structures:** sorted maps, FIFO queues, hash maps, secondary indexes

**Algorithms:** price-time priority matching, FIFO sweep, market impact (VWAP)

**Systems Engineering:** cache line alignment, struct packing, O(log N) vs O(N) operation design

**Finance:** bid-ask spread, limit vs market orders, partial fills, slippage, execution reports

---