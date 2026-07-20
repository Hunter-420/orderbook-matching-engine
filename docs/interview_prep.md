# Interview Preparation Guide

This document covers every question a technical interviewer is likely to ask about this matching engine. Each answer is written in the way you should actually say it out loud — clear, structured, and showing genuine depth without over-explaining.

---

## Architecture and Design

**Q: Walk me through the high-level architecture of your matching engine.**

The engine is a single-threaded process. At startup it binds a TCP socket, creates an epoll file descriptor, and drops into an event loop. Every client connection is registered with epoll as a non-blocking file descriptor. When a 14-byte packet arrives, epoll wakes the loop. The engine reads the packet, routes it through matching logic, and immediately sends binary execution reports back. There are no threads, no mutexes, and no shared state. Everything that matters happens on one CPU core.

**Q: Why single-threaded? Wouldn't multiple threads be faster?**

For this kind of workload, single-threaded is deliberately faster. When you add threads you also add mutexes, cache line invalidations, and context switches. The matching engine's state — the bid map, ask map, memory pool — needs to be perfectly consistent across every operation. Locking it for every access would create serialisation points that are slower than just running sequentially. Real low-latency exchanges like LMAX Disruptor solve this with a lock-free ring buffer pattern, but the performance benefit comes from eliminating contention, which a single-threaded design already achieves by construction.

---

## Matching Logic

**Q: How does price-time priority work in your engine?**

Every resting order at a price level is stored as a node in a doubly linked list. The head of the list is always the oldest order at that price. When a matching incoming order arrives, the engine always matches against the head first. If the head is fully consumed, it is removed and the next node becomes the new head. This means time priority is encoded structurally in the list ordering, not in timestamps. A timestamp would require reading and comparing an 8-byte field on every match. The list structure makes priority implicit and free.

**Q: Walk me through what happens when a buy order arrives at $101 and there is a resting sell at $100.**

First the engine checks whether the incoming order's price crosses the best ask. The best ask is always `asks_.begin()` in an ordered map. Since $101 >= $100, the prices cross. The engine reads the resting order's quantity and calculates `fill = min(incoming.qty, resting.qty)`. Both quantities are decremented by that amount. A Fill record is pushed into a pending vector. If the resting order is now fully consumed, it is removed from the linked list, its slot is returned to the memory pool, and its entry in the order directory is set to INVALID. If the incoming order has remaining quantity, the loop continues checking the next best ask. If it runs out of matching ask levels, the remainder rests on the bid side.

**Q: What is a partial fill and how do you handle it?**

A partial fill happens when two orders cross but one is larger than the other. Say a buy of 100 meets a sell of 60. The sell is fully consumed (60 shares trade), and the buy's quantity is reduced to 40. A single Fill event is recorded with quantity 60. The sell node is freed. The buy node stays in the book with its updated quantity of 40, still at the head of its price level, ready to match against the next incoming sell.

---

## Memory Pool

**Q: Why build a custom memory pool instead of using `new` and `delete`?**

`new` and `delete` call into the OS allocator, which uses a heap free-list protected by a mutex. In the worst case this stall can be hundreds of microseconds. More importantly it is unpredictable — the same operation might take 200 nanoseconds one time and 5 microseconds the next, depending on heap fragmentation and lock contention. A pre-allocated pool eliminates both problems. Every allocation is a single increment of an index into a pre-filled vector. Every deallocation is a push onto a stack. Both are O(1) and never block.

**Q: How big is your pool and what happens when it runs out?**

The pool holds one million OrderNode slots. For this engine that is effectively unlimited — even at 10,000 orders per second it would take 100 seconds of trading with zero fills to exhaust it. In a production engine you would resize the pool at startup based on the expected peak order rate, or use a layered pool with a slow-path expansion that kicks in when the fast pool exhausts. In this engine, when the pool runs out, alloc() would panic or return INVALID, which the engine checks for.

**Q: How does the free-list work exactly?**

The pool maintains a separate vector of uint32_t indices. At startup it is filled with [0, 1, 2, 3, ..., 999999] in order. `alloc()` pops the last element from this vector and returns it — that is the slot index the caller should use. `free(slot)` pushes the index back onto the vector. Because it is a stack, the most recently freed slot is the first one reused, which is cache-friendly: the memory you just freed is still warm in L1 cache.

**Q: How does an OrderNode fit into a cache line?**

An OrderNode is 28 bytes: four 32-bit integer fields, one char, three bytes of padding, and two 32-bit linked-list pointer fields. A 64-byte cache line holds two full OrderNodes with 8 bytes to spare. If you used raw pointers for prev and next instead of 32-bit pool indices, each pointer would be 8 bytes, making the node 40 bytes. You would fit only one node per cache line. Over millions of traversals per second the difference is measurable.

---

## Network Layer

**Q: Why do you use epoll instead of select or poll?**

`select` and `poll` both require passing the full set of watched file descriptors on every call, which is O(n) in the number of connections. `epoll` registers file descriptors once using `epoll_ctl` and then returns only the descriptors that are ready. The `epoll_wait` call is O(1) regardless of how many total connections exist. For a server with hundreds of concurrent clients the difference is significant.

**Q: Why a fixed-width 14-byte binary protocol instead of JSON or FIX?**

A variable-length text format like JSON requires parsing: scanning for delimiters, converting ASCII digits to integers, handling whitespace. A binary packed struct is a direct memory copy. `memcpy` of 14 bytes into a struct is one or two CPU instructions. There is no branching, no character scanning, no conversion. FIX is closer to binary but still has tag-value pairs that require linear scanning. The 14-byte format lets the engine know exactly how many bytes to read for every message, which simplifies the read loop and eliminates partial-read edge cases.

**Q: What is TCP_NODELAY and why do you enable it?**

By default TCP uses Nagle's algorithm, which buffers small packets and waits to accumulate more data before sending. This reduces the number of network packets but adds latency. For a latency-sensitive protocol where every message is exactly 14 bytes and every microsecond counts, you want each packet sent immediately. Setting `TCP_NODELAY` disables Nagle's algorithm so every `send()` call results in an immediate network transmission.

**Q: What is SIGPIPE and why do you ignore it?**

When a client disconnects unexpectedly and you call `send()` to write to that closed socket, the operating system sends a SIGPIPE signal to the process. By default SIGPIPE terminates the process immediately. In an exchange engine that would be catastrophic — one disconnected client would kill the entire server. Setting `SIGPIPE` to `SIG_IGN` makes `send()` return -1 instead, and the engine handles it gracefully by closing the file descriptor.

---

## CPU Affinity and Telemetry

**Q: Why pin the process to a specific CPU core?**

The Linux scheduler moves processes between CPU cores to balance load. Every time the process moves to a different core, its working set — the order book data, the pool, the epoll state — has to be reloaded from RAM into that core's L1 and L2 caches. This can take several microseconds. By pinning the engine to core 1 with `pthread_setaffinity_np`, the scheduler is told never to move it. The data stays hot in cache between every event loop iteration.

**Q: How do you measure latency?**

The engine records a timestamp with `std::chrono::high_resolution_clock::now()` immediately after bytes arrive from the network, and another timestamp immediately after the execution report is sent back. The nanosecond difference is pushed into a pre-allocated ring buffer called `TelemetryBuffer`. When the engine shuts down on SIGINT, it sorts the buffer and prints percentiles: min, p50, p90, p99, p99.9, p99.99, and max. The ring buffer is pre-allocated so the act of recording a latency sample never causes a heap allocation.

**Q: Your p99 is around 25 microseconds. What drives that tail?**

The p50 is around 3 microseconds which is close to the raw TCP round-trip time. The p99 spike is almost certainly the OS scheduler pre-empting the process for a kernel task or interrupt handler. There are several ways to reduce this in production: using `isolcpus` to reserve the core from the kernel, using interrupt affinity to keep network interrupts on a different core, and potentially moving to kernel bypass networking like DPDK which eliminates the socket layer entirely. On this hardware without those mitigations, 25 microseconds at p99 is a reasonable result.

---

## Visualizers and Query Protocol

**Q: How do your visualizers get data from the engine without slowing it down?**

The engine intercepts query packets inside the epoll loop before they reach the matching code. When a client sends a 14-byte request with `type = 'O'` (orderbook snapshot), `'M'` (memory snapshot), or `'D'` (node data snapshot), the engine calls the appropriate snapshot function, packs the result into a binary struct, and sends it back immediately. The `continue` statement then skips the rest of the event handling — no matching logic runs, no telemetry is recorded, no execution reports are generated. The visualizer is just another TCP client whose requests happen to be handled differently.

**Q: What is the OrderNode linked list and how do you maintain it?**

Each price level in the book is a doubly linked list of OrderNodes stored as pool slot indices. When a new order rests at an existing price level, its `prev_idx` is set to the current tail's slot, and the current tail's `next_idx` is updated to point to the new node. The new node becomes the new tail. When matching removes the head, the head's `next_idx` becomes the new head, and that node's `prev_idx` is set to INVALID. When a cancel removes a node from the middle, the engine relinks `prev.next = node.next` and `node.next.prev = node.prev` before returning the slot to the pool. This chain healing must complete before the slot is freed, otherwise the freed slot might be reused by a new order while a stale pointer still refers to it.

---

## General

**Q: What would you improve if you had more time?**

Three things stand out. First, adding a market data broadcast channel so the engine pushes book updates to subscribers rather than requiring polling. Second, implementing a proper session layer with sequence numbers and replay so clients can recover from disconnections without losing fills. Third, moving to kernel bypass networking with DPDK to eliminate the last remaining source of OS-induced latency jitter. The current architecture is already correct and fast. Those additions would make it production-grade.

**Q: What was the hardest bug you encountered building this?**

The trickiest issue was chain healing during cancellation. When a cancel removes a node that is neither the head nor the tail of its price level, three pointer updates must happen atomically from the engine's perspective: `prev.next = node.next`, `node.next.prev = node.prev`, and then the slot is freed. If you free the slot before completing the relink, and another order immediately allocates that slot, you end up with a dangling pointer in the middle of the linked list. The fix was enforcing a strict ordering: always complete chain healing first, then call `pool_->free(slot)`.
