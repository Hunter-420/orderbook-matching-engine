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
    protocol.hpp                 Wire structs OrderRequest, ExecutionReport
    telemetry.hpp                TelemetryBuffer for latency sampling
  src/
    main.cpp                     Entry point, evolves across all four phases
    engine.cpp                   Matching logic and book management
    memory_pool.cpp              Pre-allocated pool implementation
  tests/
    client_simulator.py          TCP test client (supports correctness and load testing)
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

**Goal:** bring the engine online using a non-blocking architecture and a zero-copy wire protocol.

### epoll Event Loop

Phase 3 introduces an `epoll`-based event loop. The engine is single-threaded. It waits for network events and processes incoming order requests immediately. Multithreading the matching logic would require mutexes, which destroy deterministic latency and cause context switching. A single thread with `epoll` handles high-throughput matching much faster than multi-threaded locking designs.

### Fixed-Width Binary Wire Protocol

Text protocols like JSON or FIX require parsing logic (e.g. searching for delimiters, converting strings to integers). Parsing is slow and unpredictable.
This engine uses a fixed-width binary protocol. Structures are packed with `#pragma pack(push, 1)`. Deserialization is just a strict-aliasing safe `std::memcpy` casting a byte buffer to a C++ struct.

### TCP_NODELAY

Nagle's algorithm is disabled on all sockets. Without this, the OS TCP stack might wait to buffer enough bytes before sending an ExecutionReport over the wire, causing latency spikes in the tens of milliseconds.

---

## Phase 4: Hardware Isolation and Latency Measurement

**Goal:** achieve deterministic latency and measure performance in the nanosecond range.

### CPU Affinity Pinning

The Linux OS scheduler constantly pauses threads and moves them between CPU cores to balance workload. Moving cores invalidates the L1 and L2 caches, causing massive latency spikes (jitter).
The engine uses `pthread_setaffinity_np` at startup to pin itself to a specific CPU core. This guarantees that the engine remains on one core and keeps the order book hot in that core's L1/L2 cache.

### Latency Telemetry

To measure performance without impacting it, the engine uses `TelemetryBuffer`, a pre-allocated array of one million `int64_t` slots.
When a packet hits user-space, `std::chrono::high_resolution_clock` captures a start timestamp. When the resulting execution reports are pushed to the socket, an end timestamp is captured.
The difference (in nanoseconds) is recorded in the telemetry buffer in O(1) time.

### Percentile Analysis

Recording the data is fast, but sorting it is slow.
To prevent analysis from blocking the engine, the percentiles (p50, p90, p99, p99.9, p99.99) are only calculated when the engine is gracefully shut down via `SIGINT`.

You can test the latency by starting the engine in the background and running the Python client in load mode:
```bash
./matching_engine_bin &
tests/client_simulator.py --load
kill -INT $!
```

---

## References

Blueprint: `matching-engine-blueprint.md`

Article on epoll architecture: `single-threaded-epoll-matching-engine.md`

Article on binary wire protocol: `memory-alignment-wire-protocol-orderbook.md`
