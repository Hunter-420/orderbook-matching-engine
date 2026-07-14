# Low-Latency Electronic Limit Order Book Matching Engine

**Project Title:** Single-Threaded Electronic Limit Order Book Matching Engine in C++

**Description:** A from-scratch C++ matching engine that accepts buy and sell orders from real network clients, stores all order state in a pre-allocated flat memory pool, matches orders using price-time priority, speaks a fixed-width binary wire protocol, and measures its own end-to-end latency at nanosecond resolution. No heap allocations happen on the critical path. No threads share state. No garbage collector runs.

**Core Problem:** Modern software usually solves load by adding more servers. This project solves the opposite problem. The goal is to squeeze the maximum possible performance out of a single physical CPU core. Every stall, every OS call, every cache miss, and every thread context switch adds latency. This project builds a system that eliminates all of those at the source.

---

## Table of Contents

1. [Why I Am Building This](#1-why-i-am-building-this)
2. [What This Engine Actually Does](#2-what-this-engine-actually-does)
3. [The Constraints That Drive Every Decision](#3-the-constraints-that-drive-every-decision)
4. [Technical Stack](#4-technical-stack)
5. [Project Directory Layout](#5-project-directory-layout)
6. [Architecture Overview](#6-architecture-overview)
   - [Order Book Representation](#61-order-book-representation)
   - [Network Handling Model](#62-network-handling-model)
   - [Wire Protocol](#63-wire-protocol)
7. [Phase 1: In-Memory Core Engine](#7-phase-1-in-memory-core-engine)
   - [What I Am Building in Phase 1](#71-what-i-am-building-in-phase-1)
   - [Data Structures](#72-data-structures)
   - [Matching Logic](#73-matching-logic)
   - [Test Harness](#74-test-harness)
   - [What to Verify](#75-what-to-verify)
8. [Phase 2: Memory Pool Optimization](#8-phase-2-memory-pool-optimization)
   - [What I Am Fixing and Why](#81-what-i-am-fixing-and-why)
   - [Why Cache Misses Matter](#82-why-cache-misses-matter)
   - [The Memory Pool](#83-the-memory-pool)
   - [The Order Directory Upgrade](#84-the-order-directory-upgrade)
   - [How to Verify](#85-how-to-verify)
9. [Phase 3: Network Layer](#9-phase-3-network-layer)
   - [How a Packet Reaches My Program](#91-how-a-packet-reaches-my-program)
   - [Non-Blocking I/O and epoll](#92-non-blocking-io-and-epoll)
   - [The Binary Protocol](#93-the-binary-protocol)
   - [The Main Event Loop](#94-the-main-event-loop)
   - [Nagle's Algorithm Problem](#95-nagles-algorithm-problem)
   - [Test Client](#96-test-client)
10. [Phase 4: Hardware Isolation and Latency Measurement](#10-phase-4-hardware-isolation-and-latency-measurement)
    - [CPU Affinity](#101-cpu-affinity)
    - [Nanosecond Timestamps](#102-nanosecond-timestamps)
    - [Telemetry Ring Buffer](#103-telemetry-ring-buffer)
    - [Percentile Analysis](#104-percentile-analysis)
    - [Performance Tools](#105-performance-tools)
11. [End-to-End Data Flow](#11-end-to-end-data-flow)
12. [What I Expect to Learn](#12-what-i-expect-to-learn)
13. [References](#13-references)

---

## 1. Why I Am Building This

I wrote an article called [Decoupled Vector-Map Data Layout for Allocation-Free Limit Order Book](https://www.khanalnischal.com.np/writing/decoupled-vector-map-data-layout-for-allocation-free-limit-order-book) before starting this project. That article forced me to understand how an [order book](https://en.wikipedia.org/wiki/Order_book) stores data inside memory: the three-layer architecture of a flat [memory pool](https://en.wikipedia.org/wiki/Memory_pool), a sorted price map, and an order directory. I traced every insertion, every cancellation, and every match scenario with full memory snapshots.

That article was the design. This project is the construction.

The reason I am building the full engine now is that the article only covered storage and retrieval. It did not cover what connects to that storage layer: the network that delivers orders, the event loop that drives the whole system, the binary protocol that eliminates parsing overhead, and the hardware isolation that keeps latency predictable. This project adds all of that on top of the foundation the article built.

I am building this to understand low-latency systems the way you can only understand them by building one.

---

## 2. What This Engine Actually Does

A [Limit Order Book](https://en.wikipedia.org/wiki/Order_book) is the central ledger of a financial exchange. It holds two sides. The buy side holds orders from traders who want to buy a stock at a specific price or better. The sell side holds orders from traders who want to sell at a specific price or better. When a buyer's price meets a seller's price, a trade executes.

The rule governing who trades first is [price-time priority](https://en.wikipedia.org/wiki/Order_matching_system). Price comes first: among all buyers, the one willing to pay the most gets matched first. Among all sellers, the one asking the least gets matched first. Time comes second: among all orders at the same price, the order that arrived earliest gets matched first. This is the [FIFO](https://en.wikipedia.org/wiki/FIFO_(computing_and_electronics)) rule and my article covered exactly how the doubly linked list encodes this without storing timestamps.

Here is a concrete example of what the engine does:

```
State of the book:

  SELL SIDE (asks, sorted lowest price first):
    $101.00:  Order A (100 shares, arrived 10:00:01)   HEAD
              Order B (50 shares,  arrived 10:00:02)   TAIL

  BUY SIDE (bids, sorted highest price first):
    $99.00:   Order C (200 shares)

  Best ask: $101.00    Best bid: $99.00    Spread: $2.00
  No match yet. Asks are above bids.


Incoming event: Buy 120 shares at $101.00

Step 1: Best ask is $101.00. Buy price is $101.00. Prices cross. Match begins.

Step 2: Head of $101.00 ask queue is Order A (100 shares).
        Fill min(120, 100) = 100 shares. Order A is fully consumed. Remove it.
        Remaining on the incoming buy: 120 - 100 = 20 shares.

Step 3: Head advances to Order B (50 shares). Prices still cross.
        Fill min(20, 50) = 20 shares. Order B is partially filled.
        Order B stays in book with 30 shares remaining.
        Incoming buy is now fully filled.

Output:
  Execution Report 1: 100 shares @ $101.00
  Execution Report 2: 20 shares @ $101.00

Final book state:
  SELL SIDE:
    $101.00: Order B (30 shares remaining)
  BUY SIDE:
    $99.00:  Order C (200 shares, unchanged)
```

The entire matching loop, from the moment the incoming order arrives to the moment execution reports are sent, must complete in under 2 microseconds at the median.

---

## 3. The Constraints That Drive Every Decision

Five constraints define this entire project. Every architecture choice is a consequence of one or more of these.

**Constraint 1: No OS memory allocation on the critical path.**
Calling [malloc](https://en.wikipedia.org/wiki/C_dynamic_memory_allocation) or `new` during order processing asks the operating system to find free memory. The OS allocator searches its internal free-block tables, which takes anywhere from 200 to 10,000 [nanoseconds](https://en.wikipedia.org/wiki/Nanosecond) depending on heap state. The fix is the [memory pool](https://en.wikipedia.org/wiki/Memory_pool) I described in my article: allocate everything once at startup and never call the OS again during trading.

**Constraint 2: No thread synchronisation on the critical path.**
[Mutexes](https://en.wikipedia.org/wiki/Mutual_exclusion), [spinlocks](https://en.wikipedia.org/wiki/Spinlock), and [atomic compare-and-swap](https://en.wikipedia.org/wiki/Compare-and-swap) operations all add overhead and [cache coherence](https://en.wikipedia.org/wiki/Cache_coherence) traffic. The fix is a single thread that owns all state. No sharing means no synchronisation needed.

**Constraint 3: No blocking I/O.**
A blocking [recv()](https://en.wikipedia.org/wiki/Berkeley_sockets) call puts the thread to sleep until data arrives. While it sleeps, it cannot serve other clients. The fix is [epoll](https://en.wikipedia.org/wiki/Epoll) with non-blocking [file descriptors](https://en.wikipedia.org/wiki/File_descriptor). The thread only wakes when data is actually ready.

**Constraint 4: No text parsing on the critical path.**
Parsing [JSON](https://en.wikipedia.org/wiki/JSON) or [FIX protocol](https://en.wikipedia.org/wiki/Financial_Information_eXchange) messages involves string operations, branches, and often memory allocation. The fix is a fixed-width binary protocol where the incoming bytes are already shaped like the C++ struct and require zero parsing.

**Constraint 5: No scheduler interference.**
The Linux scheduler can move my thread to a different [CPU core](https://en.wikipedia.org/wiki/Multi-core_processor) at any moment. This displaces the L1 and L2 [cache](https://en.wikipedia.org/wiki/CPU_cache) and adds microseconds of cold-cache penalty on resumption. The fix is [CPU affinity](https://en.wikipedia.org/wiki/Processor_affinity) pinning. The thread is locked to one core and the scheduler never touches it.

---

## 4. Technical Stack

**Language:** C++17 or C++20. Modern C++ gives me control over memory layout, [zero-cost abstractions](https://en.cppreference.com/w/cpp/language/zero_overhead_principle), and direct access to hardware.

**Compiler:** g++ or clang++ with `-O3 -march=native`. `-O3` enables aggressive inlining and loop optimisation. `-march=native` compiles for the exact CPU in the machine, enabling [SIMD](https://en.wikipedia.org/wiki/Single_instruction,_multiple_data) instructions where available.

**Operating System:** Linux kernel 5.x or newer. [epoll](https://en.wikipedia.org/wiki/Epoll) and [pthread affinity](https://en.wikipedia.org/wiki/Processor_affinity) calls require Linux specifically.

**Build System:** Standard Makefile.

**Standard Library headers I will use:**

`<map>` and `<unordered_map>` for the sorted price index and the Phase 1 order directory.
`<vector>` for the flat memory pool arena.
`<chrono>` for nanosecond timestamp capture without kernel involvement.
`<sys/socket.h>`, `<sys/epoll.h>`, `<netinet/in.h>` for the Linux networking layer.
`<pthread.h>` and `<sched.h>` for CPU core pinning.

No external libraries. No frameworks. Only the Linux API and the C++ standard library.

---

## 5. Project Directory Layout

```
matching_engine/
├── Makefile
├── data/
│   └── input_orders.csv          Phase 1 test data
├── include/
│   ├── engine.hpp                 OrderNode, PriceLevel, Engine class declarations
│   ├── memory_pool.hpp            MemoryPool class declaration
│   ├── protocol.hpp               OrderRequest and ExecutionReport wire structs
│   └── telemetry.hpp              TelemetryBuffer class declaration
├── src/
│   ├── main.cpp                   Entry point: event loop, CPU pinning, shutdown handler
│   ├── engine.cpp                 Matching logic and book management
│   └── memory_pool.cpp            Pool implementation
└── tests/
    └── client_simulator.py        Phase 3 and Phase 4 test client
```

The Makefile:

```makefile
CXX      = g++
CXXFLAGS = -std=c++17 -O3 -march=native -Wall -Wextra -I include

TARGET = matching_engine_bin
SRCS   = src/main.cpp src/engine.cpp src/memory_pool.cpp

$(TARGET): $(SRCS)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(SRCS) -lpthread

clean:
	rm -f $(TARGET)
```

`-lpthread` links the POSIX thread library required for `pthread_setaffinity_np`.

---

## 6. Architecture Overview

### 6.1 Order Book Representation

The data structure design for the order book is exactly what I described in my [article on decoupled vector-map layout](https://www.khanalnischal.com.np/writing/decoupled-vector-map-data-layout-for-allocation-free-limit-order-book). Three layers working together:

**Layer 1 — Memory Pool:** A pre-allocated flat `std::vector` of one million `OrderNode` slots. Every live order occupies exactly one slot. The OS is asked for this memory once at startup. During trading, slot assignment is managed internally via a [free list](https://en.wikipedia.org/wiki/Free_list) in O(1) time.

**Layer 2 — Price Map:** A `std::map` that stores one entry per active price level. The entry holds `head_idx` and `tail_idx` pointing into the memory pool. The map is a [self-balancing binary tree](https://en.wikipedia.org/wiki/Self-balancing_binary_search_tree) that keeps prices sorted automatically. Finding the best price is O(1) via `begin()`. Adding or removing a price level is O(log P) where P is the number of distinct active prices, typically tens to hundreds, not millions.

**Layer 3 — Order Directory:** A flat `std::vector` indexed by OrderID. The value at each index is the pool slot where that order lives. Cancellations look up the slot in O(1), go directly to that node, heal the doubly linked chain, and return the slot to the free list.

Orders at the same price level are connected by a [doubly linked list](https://en.wikipedia.org/wiki/Doubly_linked_list) using `next_idx` and `prev_idx` fields stored as `uint32_t` slot indices rather than 64-bit pointers. This cuts link cost in half and fits more orders per [cache line](https://en.wikipedia.org/wiki/CPU_cache_line). The chain position encodes arrival time naturally: head is oldest, tail is newest, so time priority requires no timestamp storage.

The alternatives I considered and rejected:

A sorted `std::vector` of all orders requires O(N) insertion and O(N) deletion due to element shifting. Rejected.

A `std::priority_queue` finds the best price in O(1) but has no efficient arbitrary-element removal. Cancellations become O(N). Rejected.

A `std::unordered_map` keyed by price pointing to `std::list` nodes is the closest alternative but each `std::list` node is heap-allocated at a random address. Following the list during matching causes [cache misses](https://en.wikipedia.org/wiki/CPU_cache#Cache_miss) on every traversal step. Rejected.

### 6.2 Network Handling Model

I need to serve multiple network clients simultaneously without blocking and without multiple threads.

**Thread-per-connection** is the intuitive model. Each client gets a dedicated [thread](https://en.wikipedia.org/wiki/Thread_(computing)). The thread calls `recv()` and blocks until data arrives. The problem is [context switches](https://en.wikipedia.org/wiki/Context_switch). With 100 clients there are 100 threads. The OS scheduler bounces between them, each switch taking 1 to 10 microseconds and flushing the [CPU cache](https://en.wikipedia.org/wiki/CPU_cache) with the incoming thread's data. Locking is also required to protect the shared order book. Rejected.

**Thread pool with queues** distributes incoming messages across a fixed number of workers. Still requires locks on the shared queue and on the order book. Rejected.

**Single-threaded [epoll](https://en.wikipedia.org/wiki/Epoll)** is what I am using. One thread. One event loop. No locks because there is only one thread. The Linux kernel tells the thread exactly which [file descriptors](https://en.wikipedia.org/wiki/File_descriptor) have data ready. The thread reads, processes, and responds. Then it goes back to `epoll_wait()`. Zero contention. Zero context switches. The entire engine runs on one core.

### 6.3 Wire Protocol

Network messages need to be encoded for transmission. The main options:

**Text formats ([JSON](https://en.wikipedia.org/wiki/JSON), [XML](https://en.wikipedia.org/wiki/XML), [FIX](https://en.wikipedia.org/wiki/Financial_Information_eXchange))** are human-readable. The problem is parsing. Converting text fields to integers requires loops, branches, string operations, and sometimes memory allocation. A 50-byte JSON order message may require 500 CPU instructions to parse. Rejected.

**Schema compilers ([Protocol Buffers](https://en.wikipedia.org/wiki/Protocol_Buffers), [FlatBuffers](https://en.wikipedia.org/wiki/FlatBuffers))** generate efficient code but introduce external dependencies and hide the underlying mechanics. For this project I want to understand every layer. Rejected.

**Fixed-width binary** is what I am using. I design the wire message to match the in-memory C++ struct exactly. When bytes arrive from the network, I cast the buffer pointer directly to a struct pointer. No parsing at all.

```cpp
struct OrderRequest {
    char type;       // 'N' = New order, 'C' = Cancel
    int  order_id;   // 4 bytes
    int  price;      // 4 bytes, stored as integer cents
    int  quantity;   // 4 bytes
    char side;       // 'B' = Buy, 'S' = Sell
};

// After recv() fills the buffer:
const OrderRequest* req = reinterpret_cast<const OrderRequest*>(buffer);
// req->price is immediately usable. Zero parsing instructions.
```

One important note: I represent price as an integer (cents or ticks), never as a float. [Floating-point arithmetic](https://en.wikipedia.org/wiki/Floating-point_arithmetic) is non-associative and imprecise for decimal values. `$100.01` stored as a float is actually `100.00999...` internally. All real trading systems use integer price representation.

---

## 7. Phase 1: In-Memory Core Engine

### 7.1 What I Am Building in Phase 1

Phase 1 is the matching brain with nothing else attached. No networking. No performance tuning. No memory pool yet.

The sole goal of Phase 1 is correctness. Given a sequence of orders, the engine must produce exactly the right trades in exactly the right order. Speed is irrelevant here. I will use `new` and `delete` for allocation and read orders from a CSV file.

Everything in Phase 1 is a direct implementation of the data layout I described in my [decoupled vector-map article](https://www.khanalnischal.com.np/writing/decoupled-vector-map-data-layout-for-allocation-free-limit-order-book). Phase 1 is writing that design as code.

Files added in Phase 1: `data/input_orders.csv`, `include/engine.hpp`, `src/engine.cpp`, `src/main.cpp`, `Makefile`.

### 7.2 Data Structures

```cpp
static constexpr uint32_t INVALID = UINT32_MAX;

struct OrderNode {
    uint32_t order_id;
    uint32_t price;      // Integer representation, no float
    uint32_t quantity;   // Remaining quantity (decreases on partial fills)
    char     side;       // 'B' or 'S'
    uint32_t next_idx;   // Pool slot of next order at same price. INVALID if tail.
    uint32_t prev_idx;   // Pool slot of prev order at same price. INVALID if head.
};

struct PriceLevel {
    uint32_t head_idx;      // Slot of the oldest (highest priority) order
    uint32_t tail_idx;      // Slot of the newest (lowest priority) order
    uint64_t total_volume;  // Sum of all quantities at this price
};

// Bids: highest price first. bids.begin() is always the best bid.
std::map<uint32_t, PriceLevel, std::greater<uint32_t>> bids;

// Asks: lowest price first. asks.begin() is always the best ask.
std::map<uint32_t, PriceLevel, std::less<uint32_t>> asks;

// Phase 1 order directory. Will be replaced with a flat vector in Phase 2.
std::unordered_map<int, OrderNode*> order_directory;
```

`std::greater<uint32_t>` on bids means `bids.begin()` returns the highest bid price. `std::less<uint32_t>` (the default) on asks means `asks.begin()` returns the lowest ask price. Both give the best price at `begin()`.

### 7.3 Matching Logic

The matching loop is the core of the entire project:

```cpp
void Engine::process_order(OrderNode* incoming) {
    bool is_buy = (incoming->side == 'B');
    auto& opposite = is_buy ? asks : bids;

    while (incoming->quantity > 0 && !opposite.empty()) {

        auto best_it    = opposite.begin();
        uint32_t best_p = best_it->first;

        // Check if prices cross
        bool crosses = is_buy ? (incoming->price >= best_p)
                               : (incoming->price <= best_p);
        if (!crosses) break;

        PriceLevel& level   = best_it->second;
        OrderNode*  resting = pool.get(level.head_idx);  // Phase 2: via pool

        uint32_t fill = std::min(incoming->quantity, resting->quantity);
        incoming->quantity -= fill;
        resting->quantity  -= fill;
        level.total_volume -= fill;

        emit_execution_report(fill, best_p);

        if (resting->quantity == 0) {
            // Advance head to next order
            uint32_t next = resting->next_idx;
            if (next == INVALID) {
                opposite.erase(best_it);   // Price level is now empty
            } else {
                level.head_idx = next;
                get_node(next)->prev_idx = INVALID;
            }
            order_directory.erase(resting->order_id);
            delete resting;   // Phase 1 only. Phase 2 returns slot to pool.
        }
    }

    if (incoming->quantity > 0) {
        add_to_book(incoming);   // Order rests in book
    } else {
        delete incoming;   // Phase 1 only
    }
}
```

When the incoming order rests, `add_to_book` appends it to the tail of its price level queue. If the price does not exist yet, a new `PriceLevel` entry is created in the map with `head_idx` and `tail_idx` both pointing to the new node.

When I cancel an order, the sequence from my article applies exactly: look up the slot via the order directory, read `prev_idx` and `next_idx` to find neighbours, heal the chain by relinking those neighbours around the cancelled node, update `head_idx` or `tail_idx` in the map if the cancelled node was the head or tail, clear the directory entry, and only then return the slot. Chain healing always comes before recycling.

### 7.4 Test Harness

The CSV file format:

```
ORDER_ID,SIDE,PRICE,QTY,TYPE
1,B,100,200,N
2,S,101,100,N
3,S,101,50,N
4,B,101,120,N
5,C,2,0,C
```

`TYPE=N` is a new limit order. `TYPE=C` is a cancellation (only ORDER_ID matters). The `main.cpp` reads this file line by line, parses each row with `<sstream>`, calls `engine.process_order()` or `engine.cancel_order()`, and prints the book state after each event.

### 7.5 What to Verify

Before moving to Phase 2 I will verify these cases:

**Basic match:** Sell at $100 followed by buy at $100. Exactly one trade of the right quantity. Both orders removed from the book.

**Time priority:** Two sells at $101 submitted in order. Buy at $101 for total of both quantities plus more. First sell must be consumed fully before second sell is touched.

**Partial fill:** Buy for 200 shares. Sell for 50 shares arrives at matching price. 50 shares trade. Buy rests with 150 shares remaining.

**No match (spread):** Buy at $99. Sell at $101. No trade. Both rest.

**Cancellation:** Submit a buy. Cancel it by ID. Submit matching sell. No trade occurs because the buy is gone.

**Mid-queue cancellation:** Submit orders A, B, C at same price. Cancel B. Submit match. A fills first, then C. B is completely skipped. No crash, no dangling pointer.

All six must pass before Phase 2 begins.

---

## 8. Phase 2: Memory Pool Optimization

### 8.1 What I Am Fixing and Why

Phase 1 uses `new` and `delete`. The matching logic is correct but every `new` call goes through the OS memory allocator. The allocator searches its internal free-block lists, potentially extends the heap via [brk()](https://en.wikipedia.org/wiki/Sbrk) or [mmap()](https://en.wikipedia.org/wiki/Mmap), and returns a pointer. This takes anywhere from 200 nanoseconds to 10,000 nanoseconds depending on the current state of the heap.

The second problem is [spatial locality](https://en.wikipedia.org/wiki/Locality_of_reference). Heap-allocated nodes land at random addresses decided by the allocator. When the engine follows `next_idx` pointers through a price queue during matching, each hop goes to a different random address in RAM. Each of those addresses is likely to be a [cache miss](https://en.wikipedia.org/wiki/CPU_cache#Cache_miss).

Phase 2 replaces both problems with the pool design from my article.

### 8.2 Why Cache Misses Matter

The CPU does not read from RAM directly. It reads from a [cache hierarchy](https://en.wikipedia.org/wiki/CPU_cache):

```
L1 cache:   1 to 2 nanoseconds    32 to 64 KB   holds roughly 1,000 OrderNodes
L2 cache:   4 nanoseconds         256 to 512 KB
L3 cache:   13 nanoseconds        4 to 32 MB
RAM:        67 nanoseconds        gigabytes

Cache miss cost: 67 nanoseconds.
Cache hit cost:  1 nanosecond.
Ratio: 67x slower per miss.
```

A [cache line](https://en.wikipedia.org/wiki/CPU_cache#Cache_entries) is 64 bytes. When the CPU reads any address in RAM, it fetches the full 64-byte block containing that address into L1. Adjacent bytes arrive for free.

With heap-allocated nodes scattered at random addresses, every step through a price queue is a likely cache miss. 10 nodes in a queue means up to 10 separate RAM fetches at 67 nanoseconds each. With the memory pool, all nodes live in one flat block. Even if the logical chain jumps from slot 0 to slot 9, both are within the same contiguous region. The CPU [hardware prefetcher](https://en.wikipedia.org/wiki/Instruction_prefetching) often loads nearby slots before they are even requested.

The `uint32_t` index fields I chose in my article (4 bytes each) instead of raw 64-bit pointers (8 bytes each) also matter here. Each `OrderNode` is 20 bytes with `uint32_t` links. Each cache line fits 3 orders. With `uint64_t` links each node would be 28 bytes and a cache line would fit only 2 orders. Over millions of traversals per second, fitting 3 orders per cache line instead of 2 is measurable.

### 8.3 The Memory Pool

```cpp
class MemoryPool {
public:
    static constexpr uint32_t CAPACITY = 1'000'000;
    static constexpr uint32_t INVALID  = UINT32_MAX;

    MemoryPool() : arena_(CAPACITY), next_free_(0) {
        // At startup, chain all slots into one long free list.
        for (uint32_t i = 0; i < CAPACITY - 1; ++i)
            arena_[i].next_idx = i + 1;
        arena_[CAPACITY - 1].next_idx = INVALID;
    }

    // Claim a slot. O(1). No OS call.
    uint32_t alloc() {
        if (next_free_ == INVALID)
            throw std::runtime_error("Pool exhausted");
        uint32_t slot = next_free_;
        next_free_    = arena_[slot].next_idx;
        return slot;
    }

    // Return a slot. O(1). No OS call.
    void free(uint32_t slot) {
        arena_[slot].next_idx = next_free_;  // Link into chain first
        next_free_            = slot;         // Then move the head
    }

    OrderNode& get(uint32_t slot) { return arena_[slot]; }

private:
    std::vector<OrderNode> arena_;
    uint32_t               next_free_;
};
```

`alloc()` does three things: reads `next_free_`, saves it as the slot to return, advances `next_free_` to the next available slot. No searching. No OS call.

`free()` does two things in the exact order I described in my article: link the returned slot to the front of the chain first, then update `next_free_`. Reversing these two steps loses all previously available slots from the chain permanently.

The `arena_` is allocated once in the constructor via `std::vector`. The OS is asked for memory exactly once. Every slot allocation and deallocation during trading is pure integer arithmetic inside the already-owned block.

In the engine, every `new OrderNode()` becomes `pool.alloc()` followed by writing fields into `pool.get(slot)`. Every `delete node` becomes the chain-healing sequence followed by `pool.free(slot)`.

### 8.4 The Order Directory Upgrade

Phase 1 used `std::unordered_map<int, OrderNode*>` as the order directory. Looking up an OrderID in an `unordered_map` requires hashing the key, indexing into a bucket array, and potentially traversing a collision chain.

Phase 2 replaces this with a flat `std::vector<uint32_t>` indexed directly by OrderID:

```cpp
std::vector<uint32_t> order_directory(MAX_ORDER_ID, INVALID);

// Register:
order_directory[order_id] = slot;

// Lookup:
uint32_t slot = order_directory[order_id];  // One index calculation. O(1).

// Clear on cancel:
order_directory[order_id] = INVALID;
```

A flat vector lookup is a single multiply-and-add address calculation. There is no hashing, no bucket traversal, no collision handling. For a cancellation on the hot path, the difference between a hash map lookup and a direct vector index is the difference between roughly 50 nanoseconds and roughly 1 nanosecond.

### 8.5 How to Verify

After Phase 2 is complete I will run the engine under [strace](https://en.wikipedia.org/wiki/Strace) to confirm that no OS memory calls happen during the processing loop:

```bash
strace -e trace=brk,mmap,munmap -c ./matching_engine_bin < data/input_orders.csv
```

The output shows a count of each system call. At startup I expect `brk` and `mmap` calls from the C++ runtime and from `std::vector` constructing the arena. After startup, during the matching loop, those counts must not increase. If they do, there is a hidden `new` call somewhere in the hot path that I missed.

---

## 9. Phase 3: Network Layer

### 9.1 How a Packet Reaches My Program

Before writing a single line of networking code I need to understand the journey of a network packet from the client to my `recv()` buffer. This determines what I can and cannot control.

```
Step 1: Client calls send() with order bytes.
        Kernel copies those bytes into the client-side socket send buffer.

Step 2: TCP/IP stack fragments the data and transmits over the network.

Step 3: My server's NIC receives the Ethernet frames.
        NIC uses DMA to copy bytes into kernel memory ring buffer.
        No CPU involvement yet.

Step 4: Linux kernel processes incoming frames.
        Strips Ethernet, IP, and TCP headers.
        Deposits payload bytes into the socket's receive buffer in kernel memory.

Step 5: Socket receive buffer in kernel memory.
        Bytes wait here until my program reads them.
        If buffer fills, TCP applies backpressure to the sender.

Step 6: My program calls recv(fd, buffer, size, 0).
        Kernel copies bytes from socket receive buffer into my user-space buffer.
        This is a system call: it crosses the kernel/user-space boundary.

Step 7: My buffer in user-space now holds the raw bytes.
        I cast the buffer pointer to OrderRequest* and read fields directly.
```

Steps 1 through 5 happen in kernel space. I have no control over them. My program only acts at step 6. This crossing from kernel to user-space is the [system call](https://en.wikipedia.org/wiki/System_call) overhead I cannot eliminate without kernel bypass technologies like [DPDK](https://en.wikipedia.org/wiki/Data_Plane_Development_Kit).

### 9.2 Non-Blocking I/O and epoll

By default, `recv()` is blocking. If there is no data in the socket receive buffer, the call does not return. The thread sleeps. While it sleeps it cannot serve other clients or process any orders.

I switch sockets to [non-blocking](https://en.wikipedia.org/wiki/Asynchronous_I/O) mode:

```cpp
int flags = fcntl(fd, F_GETFL, 0);
fcntl(fd, F_SETFL, flags | O_NONBLOCK);
```

With non-blocking mode, if no data is ready, `recv()` returns immediately with `errno` set to `EAGAIN`. But now I have a new problem: if I loop through all clients calling `recv()` on each one to check for data, I waste CPU cycles on the 90 clients that have nothing to send just to find the 10 that do.

[epoll](https://en.wikipedia.org/wiki/Epoll) is the Linux kernel mechanism that solves this. I register file descriptors with the kernel. The kernel monitors them. When I call `epoll_wait()`, the kernel returns only the file descriptors that currently have data ready. No wasted checking.

```cpp
// Create an epoll instance
int epfd = epoll_create1(0);

// Register a socket with epoll
epoll_event ev;
ev.events  = EPOLLIN;  // Notify me when this fd has data to read
ev.data.fd = client_fd;
epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &ev);

// Wait for events
epoll_event events[64];
int n = epoll_wait(epfd, events, 64, -1);  // -1 means wait forever

for (int i = 0; i < n; ++i) {
    int fd = events[i].data.fd;
    // fd has data ready. recv() will not block.
}
```

### 9.3 The Binary Protocol

The two message types the engine uses:

```cpp
// Incoming from client to engine
struct __attribute__((packed)) OrderRequest {
    char type;       // 'N' = New order, 'C' = Cancel
    int  order_id;
    int  price;      // Integer price (cents or ticks)
    int  quantity;
    char side;       // 'B' = Buy, 'S' = Sell
};

// Outgoing from engine to client
struct __attribute__((packed)) ExecutionReport {
    char     type;         // 'E' = execution
    int      order_id;
    int      filled_qty;
    int      fill_price;
    char     status;       // 'F' = full fill, 'P' = partial fill
};
```

`__attribute__((packed))` tells the compiler not to add padding bytes between fields. This ensures the byte layout matches exactly what the client sends.

After `recv()` fills my buffer:

```cpp
const OrderRequest* req = reinterpret_cast<const OrderRequest*>(network_buffer);
```

One pointer cast. No string operations. No loops. No allocation. The price field is immediately usable as an integer.

### 9.4 The Main Event Loop

```cpp
// Server setup
int listen_fd = socket(AF_INET, SOCK_STREAM, 0);

int opt = 1;
setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
setsockopt(listen_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

sockaddr_in addr{};
addr.sin_family      = AF_INET;
addr.sin_addr.s_addr = INADDR_ANY;
addr.sin_port        = htons(9000);
bind(listen_fd, (sockaddr*)&addr, sizeof(addr));
listen(listen_fd, 128);
set_nonblocking(listen_fd);

int epfd = epoll_create1(0);
epoll_event ev;
ev.events  = EPOLLIN;
ev.data.fd = listen_fd;
epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &ev);

// Main event loop
epoll_event events[64];
while (true) {
    int n = epoll_wait(epfd, events, 64, -1);
    for (int i = 0; i < n; ++i) {
        int fd = events[i].data.fd;

        if (fd == listen_fd) {
            // New client connecting
            int client_fd = accept(listen_fd, nullptr, nullptr);
            set_nonblocking(client_fd);
            setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
            ev.events  = EPOLLIN;
            ev.data.fd = client_fd;
            epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &ev);

        } else {
            // Existing client sent an order
            char buf[sizeof(OrderRequest)];
            int bytes = recv(fd, buf, sizeof(buf), 0);

            if (bytes <= 0) {
                epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
                close(fd);
                continue;
            }

            const OrderRequest* req = reinterpret_cast<const OrderRequest*>(buf);

            if (req->type == 'N')
                engine.process_order(req->order_id, req->side, req->price, req->quantity);
            else if (req->type == 'C')
                engine.cancel_order(req->order_id);

            ExecutionReport report = build_report(req);
            send(fd, &report, sizeof(report), 0);
        }
    }
}
```

### 9.5 Nagle's Algorithm Problem

I set `TCP_NODELAY` on every socket. This is not optional for a low-latency engine.

[Nagle's Algorithm](https://en.wikipedia.org/wiki/Nagle%27s_algorithm) is a TCP optimisation from 1984. It buffers small outgoing messages and waits up to 40 milliseconds before sending, hoping to combine them into a larger TCP segment. This is good for throughput on slow connections. It is catastrophic for latency.

Combined with [TCP Delayed Acknowledgments](https://en.wikipedia.org/wiki/TCP_delayed_acknowledgment) (the receiver waits up to 40 ms before sending an ACK), the two together can produce 40ms + 40ms = 80ms of added latency on small response messages. In an engine targeting sub-microsecond processing, an 80ms spike from a disabled socket flag is unacceptable.

```cpp
int flag = 1;
setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
```

This forces the kernel to transmit every send call as its own TCP segment immediately.

### 9.6 Test Client

```python
import socket
import struct
import time

HOST = '127.0.0.1'
PORT = 9000

def make_order(type_char, order_id, price, qty, side):
    return struct.pack('ciiic',
        type_char.encode(),
        order_id,
        price,
        qty,
        side.encode())

with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
    s.connect((HOST, PORT))
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

    # Sell 100 shares at $101
    s.sendall(make_order('N', 1, 101, 100, 'S'))
    time.sleep(0.001)

    # Buy 100 shares at $101. Should match.
    s.sendall(make_order('N', 2, 101, 100, 'B'))

    data = s.recv(1024)
    print(f"Received {len(data)} bytes")
```

The struct format `'ciiic'` must match the byte layout of `OrderRequest` exactly. If there is a mismatch the engine will read garbage values. I will verify with `sizeof(OrderRequest)` on the C++ side and `struct.calcsize('ciiic')` on the Python side.

---

## 10. Phase 4: Hardware Isolation and Latency Measurement

### 10.1 CPU Affinity

After Phase 3 the engine is functionally correct and network-connected. Phase 4 makes it deterministically fast and gives me the tools to prove it.

The Linux scheduler runs many threads across available CPU cores. When it decides to run a different thread on my core, a [context switch](https://en.wikipedia.org/wiki/Context_switch) happens. The scheduler saves all CPU registers for my thread, loads another thread, runs it, and eventually gives control back to my thread. The direct cost of this switch is 1 to 10 microseconds.

The indirect cost is worse. The incoming thread runs its own code and fills L1 and L2 cache with its data. When my engine resumes, its order book data is no longer in cache. Every memory access triggers a cache miss until the cache warms back up. On a cold cache this adds tens of microseconds of recovery time.

[CPU affinity](https://en.wikipedia.org/wiki/Processor_affinity) pins my thread to one specific core. The scheduler will never move it. No other thread can be scheduled on that core while mine is running.

```cpp
void pin_to_core(int core_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
}

// First call in main(), before everything else:
pin_to_core(1);  // Core 0 is left for the OS. Core 1 is the engine core.
```

### 10.2 Nanosecond Timestamps

```cpp
using Clock = std::chrono::high_resolution_clock;
using TP    = std::chrono::time_point<Clock>;

inline TP now() {
    return Clock::now();
}

inline int64_t ns_between(TP start, TP end) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
}
```

`std::chrono::high_resolution_clock` maps to `CLOCK_MONOTONIC` on Linux, which reads the CPU's [timestamp counter](https://en.wikipedia.org/wiki/Time_Stamp_Counter). It is monotonic (never goes backwards), not affected by NTP adjustments, and takes only a few nanoseconds to read.

I never use `CLOCK_REALTIME` for latency measurement. NTP can adjust the wall clock backwards at any moment, which would produce negative latency values.

### 10.3 Telemetry Ring Buffer

I cannot print latency numbers inside the hot loop. `printf()` eventually calls `write()`, which is a system call that can take thousands of nanoseconds. Calling it on every order would ruin the measurement and the performance.

Instead I record timestamps into a pre-allocated in-memory array. After the load test ends, I flush the array once.

```cpp
class TelemetryBuffer {
public:
    static constexpr size_t CAPACITY = 1'000'000;

    void record(int64_t latency_ns) {
        if (count_ < CAPACITY)
            samples_[count_++] = latency_ns;
    }

    void dump_percentiles() const {
        if (count_ == 0) return;
        std::vector<int64_t> sorted(samples_.begin(), samples_.begin() + count_);
        std::sort(sorted.begin(), sorted.end());

        auto pct = [&](double p) -> int64_t {
            return sorted[static_cast<size_t>(p / 100.0 * (count_ - 1))];
        };

        std::cout << "p50:    " << pct(50)    << " ns\n";
        std::cout << "p99:    " << pct(99)    << " ns\n";
        std::cout << "p99.9:  " << pct(99.9)  << " ns\n";
        std::cout << "p99.99: " << pct(99.99) << " ns\n";
    }

private:
    std::array<int64_t, CAPACITY> samples_{};
    size_t count_ = 0;
};
```

In the event loop I wrap each order processing cycle:

```cpp
auto t_start = now();
// ... recv, match, send ...
auto t_end = now();
telemetry.record(ns_between(t_start, t_end));
```

`t_start` is captured immediately after `recv()` returns. `t_end` is captured immediately after `send()` returns. The difference is the full end-to-end processing latency for that order.

### 10.4 Percentile Analysis

The project targets:

```
p50 (median):      under 2,000 nanoseconds (2 microseconds)
p99:               under 5,000 nanoseconds
p99.9:             under 10,000 nanoseconds
```

p50 tells me what a typical order experiences. Most orders hit a warm cache and process in a few hundred nanoseconds. The median should be fast.

p99 tells me that 1 in 100 orders takes longer. This is where occasional [cache misses](https://en.wikipedia.org/wiki/CPU_cache#Cache_miss) and OS interrupts appear.

p99.9 tells me the worst-case experience for 1 in 1,000 orders. This is where context switches, [TLB misses](https://en.wikipedia.org/wiki/Translation_lookaside_buffer), and any remaining heap allocations would show up if I had not eliminated them.

The gap between p50 and p99.9 is the [latency tail](https://en.wikipedia.org/wiki/Long-tail_traffic). A well-engineered engine has a tight tail. All the work in Phases 2, 3, and 4 specifically compresses this tail.

### 10.5 Performance Tools

**[strace](https://en.wikipedia.org/wiki/Strace)** counts system calls:

```bash
strace -c ./matching_engine_bin
```

This shows a table with every system call made, how many times, and total time spent. I use this to confirm zero `brk` or `mmap` calls during the processing loop.

**[perf](https://en.wikipedia.org/wiki/Perf_(Linux))** reads hardware performance counters:

```bash
# Summary statistics
perf stat -e cache-misses,cache-references,instructions,cycles ./matching_engine_bin

# Find which functions cause the most cache misses
perf record -e cache-misses ./matching_engine_bin
perf report
```

Cache miss rate = cache-misses divided by cache-references. During steady-state matching, this should be under 1%. If it is above 10%, the data layout needs investigation.

**[taskset](https://linux.die.net/man/1/taskset)** as an alternative to affinity in code:

```bash
taskset -c 1 ./matching_engine_bin
```

Pins the process to core 1 from the command line without modifying the source code. Useful for quick testing.

---

## 11. End-to-End Data Flow

This is the full journey of one order from the client to the execution report, showing every layer the order passes through:

```
CLIENT APPLICATION
        |
        | TCP connection (binary-encoded OrderRequest bytes)
        v
LINUX KERNEL TCP STACK
        |
        | Ethernet frames received by NIC
        | NIC DMA-copies payload into kernel socket receive buffer
        v
epoll_wait()
        |
        | Returns: file descriptor 7 has data ready
        v
recv(fd=7, buffer, sizeof(OrderRequest), 0)
        |
        | Copies bytes from kernel buffer into my stack buffer
        | One system call crossing the kernel/user-space boundary
        v
reinterpret_cast<const OrderRequest*>(buffer)
        |
        | Zero parsing. The bytes are already the struct.
        v
Telemetry: t_start = now()
        |
        v
Engine::process_order() or Engine::cancel_order()
        |
        +--- New order:
        |        MemoryPool::alloc()           O(1), no OS call
        |        Write OrderNode fields
        |        Search opposite side map      O(log P), P = active price levels
        |        Matching loop:
        |            Compare prices
        |            Fill min(incoming_qty, resting_qty) shares
        |            Update quantities
        |            If resting fully filled:
        |                Heal doubly linked chain
        |                Remove from Price Map if price level empty
        |                Clear Order Directory entry
        |                MemoryPool::free()    O(1), no OS call
        |        If incoming qty remaining:
        |            Append to tail of its price level queue
        |            Update Price Map tail_idx
        |            Set Order Directory entry
        |
        +--- Cancel order:
                 order_directory[order_id]       O(1), flat vector lookup
                 pool.get(slot)                  O(1), index calculation
                 Heal chain (neighbours relink)
                 Update Price Map head/tail if needed
                 Clear Order Directory entry
                 MemoryPool::free(slot)          O(1), no OS call
        |
        v
Build ExecutionReport struct
        |
        v
send(fd, &report, sizeof(report), 0)
        |
        | TCP_NODELAY ensures immediate transmission
        v
Telemetry: t_end = now()
           telemetry.record(t_end - t_start)
        |
        v
epoll_wait()   (thread goes back to waiting)
        |
        v
CLIENT APPLICATION receives ExecutionReport
```

Everything from `recv()` to `send()` must complete in under 2 microseconds at the median. No OS calls, no blocking, no thread switches, no heap allocations happen in that window.

---

## 12. What I Expect to Learn

**Memory management below the standard library.** I already understand the three-layer pool design from my article. Building it in code will teach me exactly how pre-allocation eliminates OS involvement and why the order of operations in the free list matters for correctness.

**How the Linux network stack works.** I will understand exactly where my program sits in the packet delivery chain, what the kernel does before `recv()` is called, and what the actual cost of a system call crossing the user/kernel boundary is.

**Single-threaded event-driven architecture.** The epoll pattern is the foundation of most high-performance network servers. Understanding it deeply, including why it scales better than threading under low-latency constraints, is something I can only fully grasp by building it myself.

**Binary protocol design.** Designing a wire format where the incoming bytes already match the C++ struct will make me think carefully about struct layout, alignment, padding, and byte ordering. These are skills that matter in any systems work.

**Latency measurement.** Capturing timestamps, storing them in a ring buffer, and computing percentile distributions from the results will teach me how to think about p50 vs p99.9, what tail latency means, and how to use perf and strace to find where that tail comes from.

**Performance intuition.** After this project I will be able to look at a piece of C++ code and reason about whether it will allocate, whether it will miss cache, whether it will invoke the OS, and roughly how many nanoseconds each operation costs. That kind of intuition is what separates systems engineers who can build fast code from those who can only guess.

---

## 13. References

**My related article:**

[Decoupled Vector-Map Data Layout for Allocation-Free Limit Order Book](https://www.khanalnischal.com.np/writing/decoupled-vector-map-data-layout-for-allocation-free-limit-order-book) — The foundation article I wrote before starting this project. Covers the three-layer data layout, every insertion and cancellation scenario with full memory snapshots, and the cache analysis behind the uint32 index decision.

**Data structures:**

[Order book](https://en.wikipedia.org/wiki/Order_book) — Wikipedia overview of the limit order book and how exchanges use it.

[Doubly linked list](https://en.wikipedia.org/wiki/Doubly_linked_list) — The structure used to chain orders within each price level.

[Self-balancing binary search tree](https://en.wikipedia.org/wiki/Self-balancing_binary_search_tree) — The internal structure of std::map used for the price index.

[Red-black tree](https://en.wikipedia.org/wiki/Red%E2%80%93black_tree) — The specific tree type std::map uses internally.

[Free list](https://en.wikipedia.org/wiki/Free_list) — The mechanism the memory pool uses to track available slots.

[FIFO](https://en.wikipedia.org/wiki/FIFO_(computing_and_electronics)) — The time priority rule applied within each price level.

**Memory management:**

[Memory pool](https://en.wikipedia.org/wiki/Memory_pool) — Pre-allocated block of memory managed without OS involvement.

[Dynamic memory allocation](https://en.wikipedia.org/wiki/Dynamic_memory_allocation) — Why calling new and delete on the hot path causes unpredictable latency.

[Memory fragmentation](https://en.wikipedia.org/wiki/Fragmentation_(computing)) — Why fragmentation is mostly harmless in a pre-allocated pool design.

[Memory corruption](https://en.wikipedia.org/wiki/Memory_corruption) — What happens when deletion order is wrong during cancellation.

[Dangling pointer](https://en.wikipedia.org/wiki/Dangling_pointer) — The specific bug caused by recycling a slot before healing its neighbours.

**CPU and hardware:**

[CPU cache](https://en.wikipedia.org/wiki/CPU_cache) — The cache hierarchy and why spatial locality matters.

[Cache line](https://en.wikipedia.org/wiki/CPU_cache#Cache_entries) — The 64-byte unit of cache access that determines how many OrderNodes are loaded per fetch.

[Cache miss](https://en.wikipedia.org/wiki/CPU_cache#Cache_miss) — The 67 nanosecond penalty for reading from RAM versus 1 nanosecond from L1 cache.

[Locality of reference](https://en.wikipedia.org/wiki/Locality_of_reference) — Spatial and temporal locality, and why flat arrays exploit it better than scattered heap nodes.

[Processor affinity](https://en.wikipedia.org/wiki/Processor_affinity) — Pinning a thread to a specific CPU core to prevent scheduler interference.

[Context switch](https://en.wikipedia.org/wiki/Context_switch) — The OS operation that moves a thread between cores and displaces its cache state.

[Time Stamp Counter](https://en.wikipedia.org/wiki/Time_Stamp_Counter) — The CPU hardware counter that std::chrono::high_resolution_clock reads on Linux.

[Translation lookaside buffer](https://en.wikipedia.org/wiki/Translation_lookaside_buffer) — The TLB whose invalidation adds additional cost to context switches.

[Hardware prefetcher](https://en.wikipedia.org/wiki/Instruction_prefetching) — The CPU mechanism that pre-loads nearby cache lines during sequential access.

**Networking:**

[epoll](https://en.wikipedia.org/wiki/Epoll) — The Linux I/O event notification mechanism used in the main event loop.

[Berkeley sockets](https://en.wikipedia.org/wiki/Berkeley_sockets) — The socket API used for TCP communication.

[File descriptor](https://en.wikipedia.org/wiki/File_descriptor) — The integer handle the kernel uses to represent open sockets and files.

[Nagle's algorithm](https://en.wikipedia.org/wiki/Nagle%27s_algorithm) — The TCP batching optimisation that must be disabled with TCP_NODELAY.

[TCP delayed acknowledgment](https://en.wikipedia.org/wiki/TCP_delayed_acknowledgment) — The receiver-side ACK batching that combines with Nagle to produce 40ms to 80ms latency spikes.

[Asynchronous I/O](https://en.wikipedia.org/wiki/Asynchronous_I/O) — Non-blocking I/O model where the thread is never blocked waiting for data.

[System call](https://en.wikipedia.org/wiki/System_call) — The boundary crossing between user-space and kernel-space that recv() and send() require.

[Mmap](https://en.wikipedia.org/wiki/Mmap) — The system call used by the OS allocator to extend heap memory, which strace monitors to verify pool correctness.

[Data Plane Development Kit (DPDK)](https://en.wikipedia.org/wiki/Data_Plane_Development_Kit) — Kernel bypass technology used in production HFT systems to eliminate the network stack entirely.

**Trading systems:**

[High-frequency trading](https://en.wikipedia.org/wiki/High-frequency_trading) — The category of trading that this engine's performance characteristics are designed for.

[Order matching system](https://en.wikipedia.org/wiki/Order_matching_system) — The class of systems this project belongs to.

[Bid-ask spread](https://en.wikipedia.org/wiki/Bid%E2%80%93ask_spread) — The gap between the best bid and best ask that must close before a trade executes.

[Financial Information eXchange (FIX)](https://en.wikipedia.org/wiki/Financial_Information_eXchange) — The industry standard text protocol this project intentionally avoids.

**C++ and compiler references:**

[std::vector](https://en.cppreference.com/w/cpp/container/vector) — The flat contiguous container used for the memory pool arena and order directory.

[std::map](https://en.cppreference.com/w/cpp/container/map) — The sorted associative container used for the price index.

[uint32_t](https://en.cppreference.com/w/cpp/types/integer) — The fixed-width integer type used for pool slot indices.

[reinterpret_cast](https://en.cppreference.com/w/cpp/language/reinterpret_cast) — The C++ cast used to view the network buffer as an OrderRequest struct without copying.

[Zero-overhead principle](https://en.cppreference.com/w/cpp/language/zero_overhead_principle) — The C++ design philosophy behind using abstractions that compile away entirely.

[Floating-point arithmetic](https://en.wikipedia.org/wiki/Floating-point_arithmetic) — Why prices must never be stored as floats in a trading system.

**Linux tools:**

[strace](https://en.wikipedia.org/wiki/Strace) — System call tracer used to verify zero heap allocations on the hot path.

[perf](https://en.wikipedia.org/wiki/Perf_(Linux)) — Linux profiling tool for reading hardware performance counters.

[taskset](https://linux.die.net/man/1/taskset) — Command-line tool for setting CPU affinity without modifying source code.
