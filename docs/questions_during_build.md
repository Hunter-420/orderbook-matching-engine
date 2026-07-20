# Questions During Build

These are the real technical questions I ran into while building this matching engine, along with how I worked through them and why I made the design choices I did.

## Why use integer cents instead of floats for price?

I initially considered using `double` for prices, but floating-point math introduces rounding errors. The number `$100.01` cannot be represented exactly in binary floating point. It becomes `100.00999...`. In a matching engine, a price comparison like `if (buy_price >= sell_price)` determines who gets filled. If `100.01` is stored as slightly less than it should be, an order might not cross the spread when it absolutely should. 

By storing prices as `uint32_t` integer cents (e.g., `$101.00` becomes `10100`), I guarantee that every comparison is exact, deterministic, and fast.

## Why use pool slot indices (`uint32_t`) instead of raw pointers (`OrderNode*`) for `prev` and `next`?

When I pre-allocated the `MemoryPool`, I realized that keeping the `OrderNode` size as small as possible is crucial for fitting more nodes into the CPU's L1 cache. On a 64-bit system, a raw C++ pointer is 8 bytes. My pool holds one million slots, so an index into the pool only requires a 32-bit integer (`uint32_t`), which is 4 bytes. 

By using indices instead of pointers for the linked list (`prev_idx`, `next_idx`), I saved 8 bytes per node. Multiplied by one million nodes, that's 8 megabytes of memory saved, which means a higher cache hit rate during aggressive matching loops.

## Why does the order book use `std::map` instead of `std::unordered_map`?

I use `std::map` for the price levels (bids and asks) because the engine needs to iterate through prices in order. The matching logic always needs the absolute highest bid and the absolute lowest ask. `std::map` is backed by a Red-Black tree, which keeps the keys sorted automatically. Asking a `std::map` for its `begin()` iterator is an O(1) operation that instantly yields the best price. 

An `unordered_map` (hash table) doesn't keep elements sorted. If I used a hash table, I would have to search through all keys to find the best price every time an order arrived, destroying performance.

## What happens if two orders have the same price AND the same arrival nanosecond?

In reality, they don't arrive at the exact same nanosecond because I process the `epoll` ready-list sequentially. Even if two packets hit the network card simultaneously, the kernel buffers them sequentially, and `recv()` pulls them out sequentially. The single-threaded nature of my engine ensures that the first order processed gets appended to the tail of the linked list first, guaranteeing deterministic time priority.

## Why is chain healing required before `pool->free(slot)`?

When an order is cancelled or filled, I can't just delete it. I have to heal the doubly linked list first. If node B is between node A and node C, and B is filled, I must update A's `next_idx` to point to C, and C's `prev_idx` to point to A. If I didn't do this, traversing the price level would hit a node that has already been returned to the free list, leading to memory corruption and matching failures. 

## Why does epoll use a 100ms timeout instead of -1 (infinite)?

If I used `-1`, `epoll_wait` would block forever until a socket sent data. This means the engine could not respond to graceful shutdown signals (like `SIGINT` from Ctrl+C) unless a client happened to send an order at the exact same time. By using a 100ms timeout, the loop wakes up briefly, checks a `keep_running` boolean, and shuts down cleanly if requested, allowing the telemetry buffer to dump its percentiles.

## Why ignore `SIGPIPE` instead of handling it?

When I write the Execution Report back to a client using `send()`, there is a chance the client crashed or disconnected milliseconds earlier. If I try to send data to a closed TCP socket, the Linux kernel raises a `SIGPIPE` signal. The default action for `SIGPIPE` is to kill the entire process instantly. 

I set `std::signal(SIGPIPE, SIG_IGN)` to ignore it. Now, instead of crashing, `send()` just returns `-1`. I can check that return value, log that the client disconnected, close their file descriptor, and keep serving the other clients.

## What does `TCP_NODELAY` actually prevent and what is the cost?

It prevents Nagle's Algorithm. Nagle's algorithm buffers small outgoing packets and waits up to 40ms to see if the program sends more data, so it can pack them into one larger Ethernet frame to save bandwidth. My execution reports are exactly 14 bytes. Without `TCP_NODELAY`, the kernel would trap my 14-byte fill report for 40ms before sending it to the client.

By enabling `TCP_NODELAY`, the 14-byte packet hits the wire instantly. The cost is slightly higher protocol overhead on the network (because there are more Ethernet headers per byte of payload), but for a trading engine, minimizing latency is infinitely more important than saving bandwidth.

## Why pre-allocate the telemetry buffer instead of just logging to a file?

Writing to a file requires the OS to perform I/O operations. Even if buffered, a file write can randomly trigger a disk flush that stalls the thread for thousands of microseconds. `std::cout` has the same problem. If I log the timestamp of an order while matching, the act of logging introduces latency jitter.

My `TelemetryBuffer` is a pre-allocated array of one million `int64_t` integers. Recording a timestamp requires exactly one array write (`samples_[index] = duration`) which takes less than 1 nanosecond and never blocks. I only print the data after trading is completely stopped.

## What breaks if you remove the `#pragma pack` directive from `protocol.hpp`?

If I remove `#pragma pack(push, 1)`, the C++ compiler will "pad" my structs. It does this because CPUs read memory faster when data is aligned to 4-byte or 8-byte boundaries. 

My `OrderRequest` struct has a `char` (1 byte), a `uint32_t` (4 bytes), another `uint32_t` (4 bytes), etc. Without packing, the compiler will silently insert 3 empty padding bytes after the first `char` so the next `uint32_t` lands on a clean 4-byte boundary. The struct size would silently jump from 14 bytes to 16 bytes. 

The Python client would still send exactly 14 bytes, but the C++ engine would try to read 16 bytes. The fields would completely misalign in memory, the engine would interpret the padding bytes as part of the `order_id`, and everything would crash or produce corrupt trades.
