# Single-Threaded Limit Order Book Matching Engine

A high-performance C++ matching engine built in four phases. Each phase adds one layer of the final system, from a correct in-memory core to a fully network-connected, hardware-pinned, latency-measured exchange engine.

The engine enforces price-time priority: among all resting orders at a price level, the order that arrived first is always matched first. Every design decision traces back to one goal — eliminate every unnecessary instruction on the path from incoming bytes to execution report.

---

## Project Layout

```
matching-engine/
  Makefile                       build configuration
  data/
    input_orders.csv             Phase 1 and 2 test scenarios
  include/
    types.hpp                    Shared types (OrderNode, PriceLevel, Fill)
    engine.hpp                   Engine class
    memory_pool.hpp              Pre-allocated MemoryPool class
    protocol.hpp                 Wire structs OrderRequest, ExecutionReport (added in Phase 3)
    telemetry.hpp                TelemetryBuffer for latency sampling (added in Phase 4)
  src/
    main.cpp                     Entry point, evolves across all four phases
    engine.cpp                   Matching logic and book management
    memory_pool.cpp              Pre-allocated pool implementation
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

---

## Phase 2: Memory Pool Optimization

**Goal:** eliminate OS memory allocation (heap calls) on the critical path.

### The Memory Pool

In Phase 1, `new` and `delete` were used for every `OrderNode`, invoking the OS allocator (`malloc`), which takes hundreds of nanoseconds and scatters nodes across RAM.

Phase 2 replaces this with `MemoryPool`, a pre-allocated flat `std::vector` of one million `OrderNode` slots. The OS is asked for memory exactly once at startup. 
- **Free list:** Available slots are chained using their own `next_idx` fields. 
- **O(1) allocation:** Claiming a slot simply pops the head of this free list.
- **Cache locality:** Nodes stay packed within contiguous memory, increasing the chance of L1/L2 cache hits.

### The Flat Order Directory

In Phase 1, canceling an order required an `std::unordered_map` lookup by `order_id`, which involved hashing and bucket traversal. 
Phase 2 uses a flat `std::vector<uint32_t>` indexed directly by `order_id`. A direct index lookup is a single multiply-and-add instruction, dropping lookup time from ~50ns to ~1ns.

### 4-byte Linked List

The doubly linked list used for price queues (`next_idx`, `prev_idx`) stores 32-bit slot indices instead of 64-bit raw pointers. This halves the link overhead, reducing the size of an `OrderNode` and fitting more orders into a single 64-byte cache line.

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
