# Questions During Build

These are the real technical questions I ran into while building this matching engine, along with how I worked through them and why I made the design choices I did.

---

## Why use integer cents instead of floats for price?

I initially considered using `double` for prices. The problem is that floating-point numbers in binary cannot represent all decimal fractions exactly. `$100.01` stored as a `double` becomes `100.00999999999999801...`. In a matching engine, the price comparison `if (buy_price >= sell_price)` determines who gets filled. If `100.01` is stored as slightly less than it should be, an order might not cross the spread when it absolutely should.

By storing prices as `uint32_t` integer cents (`$101.00` becomes `10100`), every comparison is exact, deterministic, and uses integer instructions which are faster than floating-point instructions.

```cpp
// Dangerous: floating-point is inexact
double price = 100.01;  // actually 100.009999999999...

// Safe: integer cents are always exact
uint32_t price_cents = 10001;  // exactly $100.01
```

---

## Why use pool slot indices (`uint32_t`) instead of raw pointers (`OrderNode*`) for `prev_idx` and `next_idx`?

On a 64-bit system, a raw pointer is 8 bytes. My `OrderNode` struct uses `uint32_t` indices (4 bytes each) for the `prev_idx` and `next_idx` linked-list fields. This saves 8 bytes per node. With one million nodes in the pool, that is 8MB of memory saved.

More importantly, keeping the struct smaller means more nodes fit inside the CPU's L1 cache (typically 32KB) at the same time. During aggressive matching loops where I am walking a linked list of orders at the same price, cache hits are critical. A cache miss costs 60 to 100 nanoseconds; a cache hit costs 1 to 4 nanoseconds.

The index into the pool arena gives me an O(1) pointer: `&pool_->arena_[idx]`. The cost is the same as a raw pointer dereference.

```cpp
// Using raw pointer: 8 bytes, points anywhere in heap (cache-unfriendly)
OrderNode* next_ptr;  // 8 bytes

// Using pool index: 4 bytes, always resolves to pool_->arena_[next_idx] (cache-friendly)
uint32_t next_idx;    // 4 bytes
// Access: OrderNode& next = pool_->get(next_idx);  // one array subscript
```

---

## Why does the order book use `std::map` instead of `std::unordered_map`?

The matching loop always needs the best bid (highest price) and best ask (lowest price). `std::map` is a Red-Black tree that keeps keys sorted. `bids_.begin()` is always the highest bid in O(1). Inserting or removing a price level is O(log N) where N is the number of distinct price levels, not the number of orders.

`std::unordered_map` is a hash table. It has O(1) average lookup by a specific key but has no ordering. Finding the maximum key requires scanning every element, which is O(N). In a live matching loop, this would be catastrophic.

```cpp
// std::map: bids_.begin() always points to the best bid. O(1).
auto it = bids_.begin();  // instantly the highest price

// std::unordered_map: no order. Must scan everything to find max. O(N).
auto max_it = std::max_element(bids_.begin(), bids_.end(), ...);  // scans all prices
```

---

## What happens if two orders have the same price AND the same arrival nanosecond?

In practice this cannot happen because the engine is strictly single-threaded. The epoll event loop processes one message at a time. Even if two TCP packets arrive simultaneously at the network card, the kernel queues them into the socket receive buffer sequentially. `epoll_wait` returns them in sequence, and `recv()` reads them one at a time. The first packet processed is always "earlier" by definition.

Even in the theoretical case where two orders had perfectly identical nanosecond timestamps, time priority is enforced by the position in the linked list, not by timestamps. The first order appended to the price level's tail will always be matched before the second one because it is closer to the head.

---

## Why is chain healing required before `pool->free(slot)`?

When an order is filled or cancelled, I must update the doubly linked list at its price level before returning the slot to the pool. If node B is between A and C:

```
A.next = B, B.prev = A
B.next = C, C.prev = B
```

When B is removed, I must set `A.next = C` and `C.prev = A`. If I skip this and call `pool->free(slot_B)` first, the pool recycles slot B for the next incoming order. Now A.next still points to what is now a different order's data. The next time I walk that price level, I read garbage data and execute phantom trades.

```cpp
// Correct order: heal the chain FIRST, then free the slot
if (node.prev != INVALID) pool_->get(node.prev).next_idx = node.next_idx;
if (node.next != INVALID) pool_->get(node.next_idx).prev_idx = node.prev_idx;
pool_->free(slot);  // Only after chain is healed
```

---

## Why does epoll use a 100ms timeout instead of -1 (infinite)?

If I used `-1`, `epoll_wait` would block forever until a socket sent data. The only way to shut down the engine cleanly would be if a client happened to send a packet at the exact moment I wanted to stop. With a 100ms timeout, the loop wakes up periodically, checks the `keep_running` boolean (set to `false` by the `SIGINT` handler), and exits gracefully. This lets the telemetry buffer dump its latency percentiles at shutdown.

```cpp
bool keep_running = true;
std::signal(SIGINT, [](int) { keep_running = false; });

while (keep_running) {
    int n = epoll_wait(epfd, events, MAX_EVENTS, 100);  // wakes every 100ms minimum
    for (int i = 0; i < n; ++i) { /* process events */ }
    // After the loop body, keep_running is checked again
}

telemetry.dump_percentiles();  // Always runs at clean shutdown
```

---

## Why ignore `SIGPIPE` instead of handling it?

When I call `send()` on a socket whose remote end has already closed, the Linux kernel raises `SIGPIPE`. The default handler for `SIGPIPE` kills the process immediately. One client crashing would take down the entire exchange.

By setting `std::signal(SIGPIPE, SIG_IGN)`, the kernel instead makes `send()` return `-1` with `errno == EPIPE`. I see that return value, close the file descriptor, remove it from epoll, and continue serving the remaining clients. The engine never crashes due to a misbehaving client.

```cpp
std::signal(SIGPIPE, SIG_IGN);  // Set before any sockets are created

// Later in the event loop:
ssize_t sent = send(fd, &report, sizeof(report), 0);
if (sent == -1) {
    // errno is EPIPE: client disconnected
    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
    close(fd);
}
```

---

## What does `TCP_NODELAY` actually prevent and what is the cost?

Nagle's Algorithm buffers small outgoing TCP packets and waits up to 40ms to see if the program sends more data, then combines them into one larger packet. This reduces the number of packets on the network (good for bandwidth) but introduces up to 40ms of latency (catastrophic for trading).

My execution report is 14 bytes. Without `TCP_NODELAY`, this 14-byte packet could sit in the kernel's send buffer for 40ms before being transmitted. Setting `TCP_NODELAY` disables Nagle's Algorithm. Every `send()` call results in an immediate IP packet.

The cost is slightly higher bandwidth usage because each 14-byte payload gets its own ~40-byte TCP/IP header. For a trading engine, this is completely acceptable.

```cpp
int opt = 1;
setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
// Now: every send() goes on the wire immediately
// Without this: 14-byte fill report could be delayed 40ms inside the kernel
```

---

## Why pre-allocate the telemetry buffer instead of writing to a log file?

Writing to a file involves a system call (`write` or `fwrite`), which transitions from user space to kernel space. Even with buffered I/O, the buffer eventually flushes, triggering a disk write. These operations take anywhere from 1,000ns to several hundred microseconds depending on filesystem state.

If I measured latency by writing to a file after each order, I would be measuring the cost of I/O, not the cost of matching. The telemetry buffer is a pre-allocated `std::array<int64_t, 1000000>`. Writing a sample is `samples_[index_++] = duration_ns`, which is a single array write taking less than 1 nanosecond. It never touches the OS during trading.

```cpp
// Bad: log file write skews the measurement
auto t_end = clock::now();
std::cout << duration_ns << "\n";  // This call can take 10,000+ ns!

// Good: pre-allocated array, sub-nanosecond write
auto t_end = clock::now();
telemetry.record(duration_ns);  // = samples_[idx++] = val. ~0.5ns.
```

---

## What breaks if you remove the `#pragma pack` directive from `protocol.hpp`?

The compiler pads struct fields to align them on their natural boundaries. A `uint32_t` field wants to start at a 4-byte-aligned address. If a `char` field comes before it, the compiler inserts 3 silent padding bytes.

Without `#pragma pack(push, 1)`, `OrderRequest` would look like this in memory:

```
char type         [1 byte] + [3 bytes padding]
uint32_t order_id [4 bytes]
uint32_t price    [4 bytes]
uint32_t quantity [4 bytes]
char side         [1 byte] + [3 bytes padding]
Total: 20 bytes (not 14)
```

The Python client always sends exactly 14 bytes. The C++ engine's `recv` call reads `sizeof(OrderRequest)` bytes, which would now be 20. The first 6 bytes of the next packet (from the next client) would be read as part of the current message. Order IDs, prices, and quantities would be completely corrupted and the exchange would execute phantom trades.

The `static_assert(sizeof(OrderRequest) == 14)` at the bottom of `protocol.hpp` would catch this at compile time and prevent the binary from building.

---

## How exactly does a partial fill work step by step?

Say the ask side has one resting order: `Alice, sell 100 shares @ $101`. Charlie places a buy for `120 shares @ $101`. Here is what happens inside `engine.cpp` on that single `new_order` call:

1. Charlie's order enters `match_incoming(side='B', price=10100, qty=120)`.
2. I check `asks_.begin()`: best ask is `$101 (10100)`. Charlie's price `10100 >= 10100`. Prices cross.
3. `resting_slot = level.head_idx` → Alice's slot.
4. `fill = std::min(120, 100) = 100`.
5. `charlie.qty = 120 - 100 = 20`. `alice.qty = 100 - 100 = 0`.
6. Alice's quantity is zero. I heal the chain (no prev, no next → erase the `$101` price level from `asks_`).
7. `order_directory_[alice.id] = INVALID`. `pool_->free(alice_slot)`.
8. Charlie still has 20 remaining. I check `asks_.begin()` again. No more asks at any price that crosses `$101` (the book is empty). Loop ends.
9. Since `charlie.qty = 20 > 0`, Charlie's remaining 20 shares rest on the bid side: `addRestingOrder(charlie.id, 'B', 10100, 20)`.

Final book state: `BID: $101 → [Charlie: 20]`, `ASK: (empty)`. One fill generated: `100 shares @ $101`.

---

## Why is there no `std::mutex` or locking anywhere in the engine?

The engine is strictly single-threaded. Only one thread ever touches the order book, the memory pool, or the order directory. The epoll event loop in `main.cpp` processes exactly one socket event at a time, sequentially. There is no scenario where two operations run concurrently.

Locks (mutexes, spinlocks) are expensive. Acquiring an uncontested `std::mutex` takes around 20 to 50 nanoseconds. Acquiring a contended mutex (one another thread is holding) can take many microseconds. In a system targeting sub-3μs median latency, even an uncontested mutex on every order would add 1 to 2μs to the critical path — a 30 to 60% increase in latency for zero benefit.

```cpp
// This engine has ZERO locks. Not a single std::mutex anywhere.
// Safety is guaranteed by architecture, not by synchronisation primitives.

// The event loop is the serialization point:
while (keep_running) {
    int n = epoll_wait(epfd, events, MAX_EVENTS, 100);
    for (int i = 0; i < n; ++i) {
        process_event(events[i]);  // One at a time. Always.
    }
}
```

---

## What is the `INVALID` sentinel value and why `UINT32_MAX`?

Throughout the engine I use `INVALID` (defined as `UINT32_MAX = 4294967295`) to represent "no slot" or "no order". I chose `UINT32_MAX` specifically because the pool only has one million slots (indices 0 to 999999). `UINT32_MAX` can never be a valid slot index, so it is a completely unambiguous sentinel. Checking `if (slot == INVALID)` has no false positives.

The alternative would be a signed type with `-1` as the sentinel. I avoided this because the pool indices are naturally unsigned (they are array subscripts), and mixing signed and unsigned arithmetic in C++ can trigger undefined behaviour or silent bugs when comparing.

```cpp
static constexpr uint32_t INVALID = std::numeric_limits<uint32_t>::max(); // 4294967295

// In addRestingOrder: head and tail start as INVALID (no nodes yet)
level.head_idx = INVALID;
level.tail_idx = INVALID;

// Checking: if (node.next_idx == INVALID) → this is the last node in the list
```

---

## How does the `node_visualizer.py` get raw struct data from the engine without modifying the trading path?

The visualizer sends a 14-byte query packet with `type = 'D'` to the engine's TCP port 9000. This packet arrives in the same `epoll_wait` loop that handles trade orders. When the engine sees `req.type == 'D'`, it calls `engine.get_node_snapshot()`, which iterates the `orderDir` map to collect the first ten active slots and packs them into a 289-byte `NodeSnapshot` struct. It sends this back over the same socket and immediately continues the event loop.

Because the visualizer uses a separate TCP connection and the engine handles it sequentially (it fully processes the query and sends the response before moving to the next `epoll_wait` event), there is zero interference with trading. The engine never pauses trading for the visualizer. If a trade order and a visualizer query arrive in the same `epoll_wait` batch, they are processed one after the other.

```python
# node_visualizer.py sends this query every 500ms:
query = struct.pack('<cIIIc', b'D', 0, 0, 0, b' ')
sock.sendall(query)
data = sock.recv(289)  # 289-byte NodeSnapshot
# Parse and display the raw struct fields
```
