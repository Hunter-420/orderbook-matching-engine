# Single-Threaded Limit Order Book Matching Engine

A high-performance C++ matching engine built in four phases. Each phase adds one layer of the final system, from a correct in-memory core to a fully network-connected, hardware-pinned, latency-measured exchange engine.

The engine enforces price-time priority: among all resting orders at a price level, the order that arrived first is always matched first. Every design decision traces back to one goal — eliminate every unnecessary instruction on the path from incoming bytes to execution report.

---

## Project Layout

```
matching-engine/
  Makefile                       build configuration
  data/
    input_orders.csv             Phase 1 test scenarios
  include/
    engine.hpp                   OrderNode, PriceLevel, Engine declarations
    memory_pool.hpp              MemoryPool class (added in Phase 2)
    protocol.hpp                 Wire structs OrderRequest, ExecutionReport (added in Phase 3)
    telemetry.hpp                TelemetryBuffer for latency sampling (added in Phase 4)
  src/
    main.cpp                     Entry point, evolves across all four phases
    engine.cpp                   Matching logic and book management
    memory_pool.cpp              Pre-allocated pool implementation (Phase 2)
  tests/
    client_simulator.py          TCP test client for Phase 3 and Phase 4
```

---

## Build

```bash
make
```

Produces `matching_engine_bin`. Requires g++ with C++17 support and Linux.

```bash
make clean
```

Removes the compiled binary.

---

## Phase 1: In-Memory Core Engine

**Goal:** correct price-time priority matching with no networking and no performance tuning. This phase proves the matching logic before any optimization is added.

### What it does

The engine holds two sides of a limit order book. The buy side sorts prices highest-first so the best bid is always at the front. The sell side sorts prices lowest-first so the best ask is always at the front. When a new order arrives, the engine checks whether prices cross. If they do, fills are generated until the incoming order is exhausted or no more crossings exist. Any remaining quantity rests in the book.

Orders at the same price are linked in a doubly linked list. Head is the oldest order, tail is the newest. This structure encodes time priority without storing timestamps: the head is always matched first.

### Data structures

```
OrderNode
  order_id    uint32_t   unique identifier
  price       uint32_t   integer cents, never float
  quantity    uint32_t   remaining shares, decreases on partial fills
  side        char       B for buy, S for sell
  next_idx    uint32_t   slot index of the next order at this price
  prev_idx    uint32_t   slot index of the previous order at this price

PriceLevel
  head_idx    uint32_t   oldest order at this price, matched first
  tail_idx    uint32_t   newest order at this price, matched last
  total_vol   uint64_t   sum of all remaining quantities at this price

Bids map      std::map with std::greater, begin() always returns best bid
Asks map      std::map with std::less, begin() always returns best ask

Order directory   std::unordered_map<order_id, OrderNode*>   Phase 1 only
```

### Phase 1 allocation model

Phase 1 uses `new` and `delete` for every `OrderNode`. This is intentional. Correctness is the only goal in Phase 1. The heap allocation cost is replaced in Phase 2.

### Running Phase 1

```bash
./matching_engine_bin < data/input_orders.csv
```

The CSV format is:

```
ORDER_ID,SIDE,PRICE,QTY,TYPE
1,S,10100,100,N     new sell order, 100 shares at $101.00
2,B,10100,100,N     new buy order, 100 shares at $101.00
2,C,0,0,C           cancel order with id 2
```

Price is stored as integer cents. `$101.00` is `10100`. TYPE is `N` for new or `C` for cancel. For a cancel row, ORDER_ID is the id of the order to cancel.

### Validation scenarios covered

**Basic match.** A sell followed by a buy at the same price. One fill is produced. Both orders are removed from the book.

**Time priority.** Two sells at the same price submitted in sequence. A buy for the combined total arrives. The earlier sell is consumed fully before the later sell is touched. Fill order follows arrival order exactly.

**Partial fill.** A buy rests in the book. A sell arrives for less than the resting buy quantity. A fill is generated. The buy remains with the reduced quantity. The sell is fully consumed.

**No match.** Buy at $99, sell at $101. Prices do not cross. Both orders rest. No fill is produced.

**Cancellation.** A buy is submitted. It is cancelled by ID. A matching sell arrives. No fill is produced because the buy is gone from the book.

**Mid-queue cancellation.** Three sells at the same price arrive in order A, B, C. B is cancelled. A buy arrives that covers A and C combined. A fills first. C fills second. B is completely skipped. No crash, no dangling pointer, no incorrect fill.

---

## Phase 2: Memory Pool

Coming in the next phase. Replaces `new` and `delete` with a pre-allocated flat array of one million `OrderNode` slots managed by a free list. Replaces the `unordered_map` order directory with a flat `vector<uint32_t>` indexed directly by order ID. Eliminates every OS memory call on the matching path.

---

## Phase 3: Network Layer

Coming in the next phase. Adds a single-threaded epoll event loop, fixed-width binary wire protocol, TCP_NODELAY on every socket, and a Python test client.

---

## Phase 4: Hardware Isolation and Latency Measurement

Coming in the next phase. Adds CPU affinity pinning, nanosecond timestamp capture, a pre-allocated telemetry ring buffer, and percentile output on shutdown.

---

## References

Blueprint: `matching-engine-blueprint.md`

Article on epoll architecture: `single-threaded-epoll-matching-engine.md`

Article on binary wire protocol: `memory-alignment-wire-protocol-orderbook.md`
