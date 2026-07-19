# Single-Threaded Limit Order Book Matching Engine

A high-performance C++ matching engine built in four phases. Each phase adds one layer of the final system, from a correct in-memory core to a fully network-connected, hardware-pinned, latency-measured exchange engine.

Project URL: [Single-Threaded Limit Order Book Matching Engine in C++](https://www.khanalnischal.com.np/projects/single-threaded-limit-order-book-matching-engine-in-c)

The engine enforces price-time priority: among all resting orders at a price level, the order that arrived first is always matched first. Every design decision traces back to one goal — eliminate every unnecessary instruction on the path from incoming bytes to execution report.

---

## Project Layout

```
matching-engine/
  Makefile                       build configuration
  docs/                          Detailed documentation and code snippets per phase
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

## Phases Overview and Test Outputs

For deep technical details and code snippets, please see the `docs/` directory. Below is a high-level overview of what each phase achieves and how its output looks.

### Phase 1: In-Memory Core Engine
**What it does:** Implements correct price-time priority matching with no networking and no performance tuning. It proves the matching logic (handling partial fills, cancellations, and order book states) is perfectly correct.
**Detailed Docs:** `docs/phase1_core_engine.md`

**Sample Output (Processing a new order):**
```text
--- Event: id=1 NEW SELL qty=100 price=101 ---
=== ORDER BOOK ===
  ASKS (lowest first):
    $101: [ord=1 qty=100]  (total=100)
  BIDS (highest first):
==================
```

### Phase 2: Memory Pool Optimization
**What it does:** Eliminates OS memory allocation (`new`/`delete`) on the critical path. It introduces a pre-allocated flat `MemoryPool` of one million slots and a flat `vector` order directory for `O(1)` order cancellation lookups.
**Detailed Docs:** `docs/phase2_memory_pool.md`

**Sample Output (Validating no allocations):**
Using `strace`, the output proves that after startup, zero heap allocation calls are made during trading:
```text
% time     seconds  usecs/call     calls    errors syscall
------ ----------- ----------- --------- --------- ----------------
  0.00    0.000000           0         0           brk
  0.00    0.000000           0         0           mmap
  0.00    0.000000           0         0           munmap
------ ----------- ----------- --------- --------- ----------------
```

### Phase 3: Network Layer
**What it does:** Brings the engine online using a non-blocking `epoll` architecture and a zero-copy fixed-width binary wire protocol. It disables Nagle's algorithm with `TCP_NODELAY` to prevent latency spikes.
**Detailed Docs:** `docs/phase3_network_layer.md`

**Sample Output (Python Client interaction):**
```text
Engine listening on port 9000
Client connected: fd 13
--- Submitting Sell 100 @ $101.00 ---
Report -> type:E, order_id:1, filled_qty:0, fill_price:0, status:A
--- Submitting Buy 100 @ $101.00 ---
Report -> type:E, order_id:2, filled_qty:0, fill_price:0, status:A
Report -> type:E, order_id:2, filled_qty:100, fill_price:101.0, status:F
Report -> type:E, order_id:1, filled_qty:100, fill_price:101.0, status:F
```

### Phase 4: Hardware Isolation and Latency Measurement
**What it does:** Achieves deterministic latency by pinning the process to a specific CPU core (`pthread_setaffinity_np`) to avoid L1/L2 cache invalidations. Measures performance in the nanosecond range using a pre-allocated `TelemetryBuffer`.
**Detailed Docs:** `docs/phase4_telemetry.md`

**Sample Output (Latency Percentiles after 1000 orders):**
```text
=== LATENCY PERCENTILES (ns) ===
Samples: 1000
Min:     732 ns
p50:     3012 ns
p90:     14638 ns
p99:     24958 ns
p99.9:   84334 ns
p99.99:  84334 ns
Max:     84334 ns
================================
```
