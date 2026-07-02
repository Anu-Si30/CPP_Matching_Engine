# MERCURY: High-Performance Electronic Exchange

A from-scratch implementation of an electronic exchange matching engine in C++, built to demonstrate the systems engineering, data structures, and market microstructure knowledge that HFT firms care about.

---

## Overview

Real exchanges like NASDAQ and NYSE are, at their core, ultra-low latency software systems that:
1. Accept orders from traders ("buy 100 shares of AAPL at $150")
2. Store unfilled orders in a data structure called an **order book**
3. Continuously check whether any two orders should trade against each other
4. When they do, generate a **fill** and notify both parties

This project builds that system from the ground up in C++, then layers trading agents, a market maker, risk management, a lock-free networking module, and a live terminal visualization on top of it.

The architecture mirrors how firms like Citadel and Virtu actually think about these systems — the exchange is the platform, and the market maker is an application that runs on it.

---

## Architecture

```text
                 Historical Data (ITCH / CSV)
                          │
                          ▼
              ┌────────────────────────┐
              │      Exchange Core     │
              │  ┌──────────────────┐  │
              │  │   Order Book     │  │
              │  │  (per symbol)    │  │
              │  └────────┬─────────┘  │
              │           │            │
              │  ┌────────▼─────────┐  │
              │  │ Matching Engine  │  │
              │  │ Price-Time FIFO  │  │
              │  └────────┬─────────┘  │
              │           │            │
              │  ┌────────▼─────────┐  │
              │  │Execution Reports │  │
              │  └──────────────────┘  │
              └───────────┬────────────┘
                          │
          ┌───────────────┼──────────────┐
          │               │              │
          ▼               ▼              ▼
     Market Maker    Random Trader  Momentum Trader
          │
          ▼
   Risk / Inventory Engine
          │
          ▼
   Terminal UI + Benchmarks
```

---

## Engineering Decisions

Engineers love design rationale. Here is why the system is built the way it is:

| Decision | Rationale |
|----------|----------|
| `alignas(64)` on `Order` | One cache line per order — halves memory fetches on the hot path. |
| `std::map` for bid/ask levels | Ordered iteration required for matching sweeps; `unordered_map` breaks this. |
| `std::deque` per price level | O(1) push_back + O(1) pop_front — FIFO without vector's O(N) shifting. |
| Secondary `order_index` (per book) | Makes cancel O(log N) instead of O(N) linear scan across all levels. |
| Object Pooling (`ObjectPool`) | Eliminates `new`/`malloc` on the hot path via lock-free spinlocks (`std::atomic_flag`). |
| Callback-based fills | Decouples engine from downstream consumers; stays testable in isolation. |
| Modify = cancel+reinsert (price/qty↑) | Prevents queue-position gaming; mirrors real exchange rules. |
| Modify = in-place (qty↓) | Rewards risk reduction — the one modification without queue penalty. |
| SPSC Lock-Free Queues | Eliminates mutex contention; enables sub-microsecond inter-thread communication. |
| ANSI Escape Sequences | `\033[2J\033[H` for ultra-fast screen redrawing without ncurses. |
| Byte-Swapping `bswap64` | Fast bitwise shifting to convert Network Big-Endian to Host Little-Endian. |
| `#pragma pack(push, 1)` | Maps raw network bytes directly into C++ structs for zero-copy parsing. |
| `static_assert(sizeof(Order)==64)` | Compile-time enforcement — layout regression breaks the build, not production. |

---

## Market Microstructure

**Data Structures:** sorted maps, FIFO queues, hash maps, secondary indexes
**Algorithms:** price-time priority matching, FIFO sweep, market impact (VWAP)
**Finance:** bid-ask spread, limit vs market orders, partial fills, slippage, execution reports

### The Order Book
The order book stores every resting (unfilled) limit order for one symbol. It has two sides:

```text
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

### Advanced Order Types
- **FOK (Fill Or Kill):** Scans the order book to ensure the entire quantity can be filled immediately. If not, it is entirely rejected without execution.
- **IOC (Immediate Or Cancel):** Sweeps what it can, and the engine automatically cancels the remainder instead of letting it rest passively in the book.
- **GTC (Good Till Cancelled):** Standard resting limit order.

### Market Maker Skew
The integrated Market Maker dynamically adjusts its price based on inventory to prevent directional exposure. If the MM buys too many shares (Long), it lowers both its bid and ask prices to incentivize selling and discourage buying.

---

## Benchmarks

This system is built for extreme performance. By heavily optimizing cache-line access and pre-allocating memory using a custom spinlock Object Pool, the engine achieves incredibly low latency.

*Hardware: Standard Consumer Laptop (Windows/MinGW GCC)*

**100,000 Orders Processed**

| Metric | Result |
|--------|--------|
| **Average Latency** | `841 ns` (0.84 μs) |
| **P50 Latency** | `700 ns` (0.7 μs) |
| **P99 Latency** | `1.89 μs` |
| **Throughput** | `9,635 orders/sec` |
| **Memory Allocation on Hot Path** | `0 bytes` |

*(Note: Throughput scaling is bounded by the single-threaded matching lock, but inter-thread communication latency is near-zero).*

---

## Screenshots

The project includes a live terminal dashboard that visualizes the order book, the spread, and the internal PnL of the trading agents in real-time.

```text
+========================================================================+
|  CPP MATCHING ENGINE -- LIVE DASHBOARD                                 |
+========================================================================+
|  SYMBOL: AAPL    |  MID: $100.09  |  SPREAD: $0.01                     |
+-------------------------+----------------------------------------------+
|       ORDER BOOK        |             TRADING AGENTS                   |
|                         |                                              |
|   $100.15 |   74 ASK    |                                              |
|   $100.14 |  100 ASK    |  [Market Maker]                              |
|   $100.12 |  141 ASK    |  Inventory:   +54 shares                     |
|   $100.11 |  149 ASK    |  Fills:       53                             |
|   $100.10 |   44 ASK    |  Total PnL:   -14970.62                      |
|   --------------------- |                                              |
|   $100.09 |   74 BID    |  [Noise Trader]                              |
|   $100.04 |  100 BID    |  Inventory:   +249 shares                    |
|   $99.97  |    6 BID    |  Fills:       242                            |
|   $99.96  |   26 BID    |  Total PnL:   -22497.52                      |
|   $99.94  |   93 BID    |                                              |
+========================================================================+
```

---

## Roadmap

### Current Features
- ✓ **Exchange Core:** Order books, price levels, secondary O(log N) cancellation index.
- ✓ **Matching Engine:** Price-time priority matching, FIFO sweeps, partial fills.
- ✓ **Advanced Orders:** Market vs Limit, FOK, IOC, GTC.
- ✓ **Trading Agents:** Market Maker, Noise Trader, Momentum Trader.
- ✓ **Risk Engine:** Pre-trade checks, open exposure position limits.
- ✓ **High Performance:** Object pooling, lock-free SPSC queues, multithreading.
- ✓ **Analytics:** VWAP, Sharpe Ratio, Max Drawdown tracking.
- ✓ **Networking:** Standalone TCP server using native WinSock to accept external orders.
- ✓ **Dashboard:** Real-time ANSI terminal UI.
- ✓ **Replay Engine:** NASDAQ ITCH 5.0 binary protocol parser.

### Planned Features
- • Fix minor edge cases in TCP socket disconnection handling.
- • Expand ITCH parser to support full book rebuilds (Add, Modify, Execute, Delete messages).
- • Create Python TCP client scripts to simulate external hedge fund connections.
- • Optimize matching engine struct packing further for AVX SIMD instructions.