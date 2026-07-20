# Single-Threaded Limit Order Book Matching Engine

A high-performance C++ matching engine built in four phases. Each phase adds one layer of the final system, from a correct in-memory core to a fully network-connected, hardware-pinned, latency-measured exchange engine.

Project URL: [Single-Threaded Limit Order Book Matching Engine in C++](https://www.khanalnischal.com.np/projects/single-threaded-limit-order-book-matching-engine-in-c)

Live Demo & Documentation Site: [hunter-420.github.io/orderbook-matching-engine](https://hunter-420.github.io/orderbook-matching-engine/)

The engine enforces price-time priority: among all resting orders at a price level, the order that arrived first is always matched first. Every design decision traces back to one goal — eliminate every unnecessary instruction on the path from incoming bytes to execution report.

---

## Project Layout

```
matching-engine/
  Makefile                       build configuration
  docs/
    phase1_core_engine.md        Matching logic, price-time priority, data structures
    phase2_memory_pool.md        Zero-allocation pool, free-list, flat order directory
    phase3_network_layer.md      epoll event loop, binary protocol, TCP_NODELAY
    phase4_telemetry.md          CPU affinity pinning, nanosecond telemetry buffer
    memory_layout.md             Memory and node visualizer internals with examples
    testing_and_simulation.md    All test tools with query protocol and examples
    questions_during_build.md    Real questions I faced and solved during the build
    learning_outcomes.md         What I learned about systems, networking, and performance
  data/
    input_orders.csv             Phase 1 and 2 test scenarios
  include/
    types.hpp                    Shared types (OrderNode, PriceLevel, Fill)
    engine.hpp                   Engine class
    memory_pool.hpp              Pre-allocated MemoryPool class
    protocol.hpp                 Wire structs and snapshot structs
    telemetry.hpp                TelemetryBuffer for latency sampling
  src/
    main.cpp                     Entry point and epoll event loop
    engine.cpp                   Matching logic, book management, snapshot methods
    memory_pool.cpp              Pre-allocated pool implementation
  tests/
    client_simulator.py          Correctness and load testing
    exchange_visualizer.py       Automated real-time ticker tape
    orderbook_visualizer.py      Live order book ladder queried from engine
    memory_visualizer.py         Live MemoryPool free-list and slot state
    node_visualizer.py           Live raw OrderNode struct data with linked-list pointers
    manual_client.py             Interactive CLI for placing orders manually
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

## Running the Visualizers

The engine is a single-process server. All visualizers connect to it as clients and poll for binary snapshots of its internal state. Start the engine once and then run each visualizer in its own terminal.

**Terminal 1:** Start the engine
```bash
./matching_engine_bin
```

![Engine terminal](docs/screenshots/matching_engine_bin.png)

**Terminal 2:** Live order book ladder (all orders from all clients)
```bash
python3 tests/orderbook_visualizer.py
```

![Order book ladder](docs/screenshots/orderbook_visualizer.png)

**Terminal 3:** MemoryPool state (free-list head, active slot count)
```bash
python3 tests/memory_visualizer.py
```

![Memory visualizer](docs/screenshots/memory_visualizer.png)

**Terminal 4:** Raw OrderNode structs (every field including linked-list pointers)
```bash
python3 tests/node_visualizer.py
```

![Node visualizer](docs/screenshots/node_visualizer.png)

**Terminal 5:** Place orders manually and watch all visualizers update in real time
```bash
python3 tests/manual_client.py
```

![Manual client](docs/screenshots/manual_client.png)

---

## Phases Overview and Test Outputs

For deep technical details and code snippets, please see the `docs/` directory. Below is a high-level overview of what each phase achieves and how its output looks.

### Phase 1: In-Memory Core Engine
**What it does:** Implements correct price-time priority matching with no networking and no performance tuning. It proves the matching logic (handling partial fills, cancellations, and order book states) is perfectly correct.
**Detailed Docs:** [docs/phase1_core_engine.md](docs/phase1_core_engine.md)

**How to generate this output:**
Start the engine, then run the manual client and place an order:
```bash
python3 tests/manual_client.py
Enter command > sell 100 101.00
```

**Real Output (Manual Client & Order Book):**
The manual client prints:
```text
Sent SELL order 1: 100 @ $101.00
[SYSTEM] Order 1 Accepted
```
Simultaneously, `python3 tests/orderbook_visualizer.py` will update to show:
```text
BID QTY       PRICE          ASK QTY
              $101.00            100
------------ spread ------------
```

### Phase 2: Memory Pool Optimization
**What it does:** Eliminates OS memory allocation (`new`/`delete`) on the critical path. It introduces a pre-allocated flat `MemoryPool` of one million slots and a flat `vector` order directory for `O(1)` order cancellation lookups.
**Detailed Docs:** [docs/phase2_memory_pool.md](docs/phase2_memory_pool.md)

**How to generate this output:**
Start the engine, then run the memory visualizer to watch the internal free-list pointer shift in real time as orders are placed and filled.
```bash
python3 tests/memory_visualizer.py
```

**Real Output (Memory Visualizer):**
```text
[MEMORY STATE]
Active Orders: 3
Free Slots:    999997
Next Free Idx: 3

Occupied Physical Slots (first 10):
Slot 0 | Slot 1 | Slot 2
```

### Phase 3: Network Layer
**What it does:** Brings the engine online using a non-blocking `epoll` architecture and a zero-copy fixed-width binary wire protocol. It disables Nagle's algorithm with `TCP_NODELAY` to prevent latency spikes.
**Detailed Docs:** [docs/phase3_network_layer.md](docs/phase3_network_layer.md)

**How to generate this output:**
Start the engine, then run the exchange visualizer which streams thousands of automated TCP binary orders.
```bash
python3 tests/exchange_visualizer.py
```

**Real Output (Exchange Visualizer processing binary network reports):**
```text
[EXCHANGE VISUALIZER] Connected to matching engine on port 9000
...
FILL  order 43  10 shares at $100.12
FILL  order 44  20 shares at $100.12
FILL  order 47  50 shares at $100.18
FILL  order 49  10 shares at $100.18
```

### Phase 4: Hardware Isolation and Latency Measurement
**What it does:** Achieves deterministic latency by pinning the process to a specific CPU core (`pthread_setaffinity_np`) to avoid L1/L2 cache invalidations. Measures performance in the nanosecond range using a pre-allocated `TelemetryBuffer`.
**Detailed Docs:** [docs/phase4_telemetry.md](docs/phase4_telemetry.md)

**How to generate this output:**
Start the engine, run the high-load client simulator to blast 1,000 orders into the engine, then press `Ctrl+C` on the engine terminal to safely shut it down and trigger the telemetry dump.
```bash
python3 tests/client_simulator.py --load
```

**Real Output (Engine Terminal on shutdown):**
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

---

## Documentation

Each document is written as a self-contained explanation with worked examples and code snippets. You do not need to read them in order, but reading them in phase order builds the full picture progressively.

| Document | What It Covers |
|---|---|
| [Phase 1: Core Engine](docs/phase1_core_engine.md) | Price-time priority matching, OrderNode, PriceLevel, the matching loop with a full Alice-Bob-Charlie walk-through |
| [Phase 2: Memory Pool](docs/phase2_memory_pool.md) | Why `new`/`delete` are eliminated, the free-list pop and push, the flat order directory, cache-line packing |
| [Phase 3: Network Layer](docs/phase3_network_layer.md) | Single-threaded epoll, 14-byte binary protocol, TCP_NODELAY, SIGPIPE handling |
| [Phase 4: Telemetry](docs/phase4_telemetry.md) | CPU affinity pinning, nanosecond timestamps, the ring buffer, percentile calculation |
| [Memory Layout](docs/memory_layout.md) | How memory and node visualizers query the engine, free-list walk-through with a live example, linked-list pointer diagrams |
| [Testing and Simulation](docs/testing_and_simulation.md) | All six test tools, the query protocol architecture, examples and expected output for each tool |
| [Questions During Build](docs/questions_during_build.md) | Real technical questions I ran into, why I chose integer cents, why I used indices instead of pointers, and network configuration choices |
| [Learning Outcomes](docs/learning_outcomes.md) | My reflections on systems programming, nanosecond performance, epoll networking, and zero-allocation architecture |
