# Learning Outcomes

Building this matching engine from scratch reshaped how I think about software. Every decision had a measurable consequence in nanoseconds. Here is what I came away with across four areas, with concrete examples from the actual codebase.

## 1. Systems Programming Concepts

**Memory is physical, not abstract.**

Before this project I treated `new` and `delete` as costless keywords. Building the `MemoryPool` in `src/memory_pool.cpp` taught me the real cost. Every `new OrderNode()` call in Phase 1 asked the OS for memory from the heap. The heap allocator must search its internal free-list, potentially lock against other threads, mark the block as used, and return a pointer. This takes anywhere from 50ns to 200,000ns depending on fragmentation.

In Phase 2 I replaced every `new` and `delete` with two array operations:

```cpp
// alloc(): one read + one write = ~1ns
uint32_t slot = next_free_;
next_free_    = arena_[slot].next_idx;

// free(): one read + one write = ~1ns
arena_[slot].next_idx = next_free_;
next_free_            = slot;
```

After startup, `strace` showed zero `brk` or `mmap` calls during trading. The OS heap was never touched again.

**Struct layout is not automatic.**

When I first wrote `OrderRequest` in `include/protocol.hpp` without `#pragma pack(push, 1)`, the Python client's 14-byte packets were being read incorrectly by the C++ engine. The compiler had silently inserted 3 padding bytes after the `char type` field so the `uint32_t order_id` would land on a 4-byte boundary. The struct was 20 bytes, not 14. The `static_assert(sizeof(OrderRequest) == 14)` I added caught this at compile time on the first attempt to build.

**Doubly linked lists encode state without storing it.**

In `include/types.hpp`, the `OrderNode` struct stores `prev_idx` and `next_idx` as `uint32_t` pool slot indices rather than raw pointers. This encodes the entire time-priority queue at each price level without a separate data structure, and saves 8 bytes per node versus raw 64-bit pointers. With one million slots, that is 8MB saved — enough to significantly increase the cache hit rate during match traversal.

## 2. Networking Architecture

**One thread can outperform many threads for this workload.**

Phase 1 processed orders from a CSV file. Phase 3 replaced that with an epoll event loop in `src/main.cpp`. Instead of spawning one thread per client (which would require a mutex around the order book), I used a single thread that processes events sequentially. The kernel's epoll mechanism tells me exactly which sockets have data ready:

```cpp
int n = epoll_wait(epfd, events, MAX_EVENTS, 100);
for (int i = 0; i < n; ++i) {
    int fd = events[i].data.fd;
    // Process one socket at a time, sequentially
}
```

Because there is only one thread, there is zero need for `std::mutex` anywhere in the codebase. The absence of locking is not laziness — it is the design guarantee that makes sub-microsecond latency achievable.

**40ms of invisible latency can come from the kernel.**

After Phase 3 was working I noticed that my Python test client was sometimes waiting over 40ms for an execution report even though the matching logic completed in under 5 microseconds. The culprit was Nagle's Algorithm: the Linux kernel was buffering my 14-byte execution report and waiting to see if more data would arrive before sending it. One `setsockopt` call fixed it:

```cpp
int opt = 1;
setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
```

After that, every 14-byte execution report hit the wire immediately.

**One crashed client must not kill the exchange.**

The first time I tested with a client that disconnected mid-session, the entire engine process crashed. `send()` on a dead socket raises `SIGPIPE`, whose default action is process termination. I added one line at the top of `main()`:

```cpp
std::signal(SIGPIPE, SIG_IGN);
```

Now `send()` returns `-1` instead of crashing. I close that client's file descriptor, call `epoll_ctl(EPOLL_CTL_DEL, ...)`, and the engine continues serving all other clients.

## 3. Performance Engineering

**Nanoseconds are real units of work.**

After Phase 4 I had a working `TelemetryBuffer` in `include/telemetry.hpp` that recorded the duration of every order from bytes-received to execution-report-sent. The output after 1000 orders:

```
p50:     3012 ns   (the typical case: matching + two sends)
p90:    14638 ns
p99:    24958 ns
p99.9:  84334 ns   (the OS scheduler interrupted us here)
```

The p99.9 spike of 84μs was not a bug in the matching logic. It was the Linux scheduler migrating the process thread to a different CPU core. When that happens, the L1 and L2 caches that held the order book data go cold. Every memory access in the next few orders must go all the way to main RAM (60 to 100ns per read instead of 1 to 4ns).

**CPU affinity pinning eliminates that spike.**

I added `pin_to_core(1)` in `src/main.cpp`:

```cpp
cpu_set_t cpuset;
CPU_ZERO(&cpuset);
CPU_SET(1, &cpuset);
pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
```

After pinning to Core 1, the engine's caches stayed warm. The p99.9 spike dropped dramatically because the OS scheduler could no longer evict the thread.

**Measuring incorrectly is worse than not measuring at all.**

My first attempt at latency measurement used `std::cout` inside the event loop: print the duration after each order. This was measuring the cost of `fwrite` and kernel buffer flushes (often 10,000ns+), not the matching engine. The `TelemetryBuffer` records samples with a single array write:

```cpp
void record(int64_t ns) {
    samples_[index_] = ns;  // ~0.5ns
    if (++index_ >= CAPACITY) { index_ = 0; wrapped_ = true; }
}
```

No system calls, no allocation, no locking. All printing is deferred to shutdown.

## 4. Software Design

**Build in phases, test each phase before adding the next.**

I built the engine in four strict phases: correctness first (Phase 1), then memory performance (Phase 2), then networking (Phase 3), then measurement (Phase 4). When a bug appeared in Phase 3 causing a wrong fill quantity, I knew immediately it was in the Phase 1 matching logic — not in the memory pool, not in the network layer — because those had already been independently verified.

Without this discipline, a fill-quantity bug in a fully-assembled system with networking, memory pooling, and telemetry all running simultaneously would have taken many times longer to isolate.

**The visualizer architecture is a clean separation of concerns.**

The five Python visualizer scripts (`orderbook_visualizer.py`, `memory_visualizer.py`, `node_visualizer.py`, `exchange_visualizer.py`, `manual_client.py`) all connect to the same port 9000 as ordinary TCP clients. The engine distinguishes between order packets and query packets by reading the first byte (`type` field). Query packets are handled and returned immediately without touching the matching logic or the telemetry buffer.

This means I can inspect every internal data structure of a live, trading engine in real time with zero impact on the hot path. The order book, pool free-list, and raw `OrderNode` structs are all observable while orders are actively being matched.
