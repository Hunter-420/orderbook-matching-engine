# Phase 3: Network Layer

The goal of Phase 3 is **connectivity**, bringing the engine online so it can receive orders over a network. We must do this without introducing context-switching overhead or slow text parsing.

## Logic Explanation: epoll and Binary Protocols
A traditional web server uses one thread per client. If two clients send orders simultaneously, both threads hit the limit order book at the same time. To prevent corruption, we would need a `std::mutex` (a lock). But locks stall the CPU, ruining latency.

Instead, we use a single thread running an `epoll` event loop. `epoll` asks the Linux kernel: "Are there any sockets with data ready to read?" The thread reads the data, matches the order, sends the fill out, and loops back around. By never multithreading the core, we **never need locks**.

Furthermore, instead of sending JSON (which requires searching for quotes and parsing strings to integers), we send **raw binary C++ structs**. We disable compiler padding so the structs are exactly 14 bytes long. When bytes arrive over the network, we don't parse them; we just cast the memory directly to our struct using `std::memcpy`.

### Example
1. **Client A** sends a binary packet (14 bytes) representing a Buy order.
2. **Client B** sends a binary packet (14 bytes) representing a Sell order.
3. The `epoll_wait` function wakes up and sees both Client A and Client B have data.
4. The engine reads Client A's 14 bytes, casts it instantly to an `OrderRequest`, and processes it in the engine.
5. The engine then reads Client B's 14 bytes, casts it, and processes it.
6. The engine matches A and B and pushes a 14-byte `ExecutionReport` to both sockets.

Everything happens sequentially on one core, blindingly fast.

## Code Snippets

### Binary Wire Protocol
Instead of sending text like `{"type": "new", "price": 101.00}`, we use raw memory structs. 

```cpp
// include/protocol.hpp
#pragma pack(push, 1) // Disable compiler padding

// Exactly 14 bytes
struct OrderRequest {
    char     type;       
    uint32_t order_id;   
    uint32_t price;      
    uint32_t quantity;   
    char     side;       
};

#pragma pack(pop)
```

By packing the struct, we guarantee its size over the wire. Deserializing is instantaneous and zero-copy using strict-aliasing safe `memcpy`:

```cpp
// src/main.cpp (snippet)
char buf[sizeof(OrderRequest)];
ssize_t bytes = recv(fd, buf, sizeof(buf), 0);

if (bytes == sizeof(OrderRequest)) {
    OrderRequest req;
    std::memcpy(&req, buf, sizeof(OrderRequest)); // 0 overhead cast
    engine.new_order(req.order_id, req.side, req.price, req.quantity);
}
```

### TCP_NODELAY
By default, the OS uses Nagle's Algorithm, which buffers small packets to send them together. For a trading engine, this introduces massive latency (often 40ms+). We turn it off on every socket.

```cpp
// src/main.cpp (snippet)
void set_nodelay(int fd) {
    int opt = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
}
```

### epoll Event Loop
We register all client sockets to `epoll`. When the OS detects data on a socket, `epoll_wait` wakes up our thread immediately.

```cpp
// src/main.cpp (snippet)
int n = epoll_wait(epfd, events, MAX_EVENTS, 100);
for (int i = 0; i < n; ++i) {
    int fd = events[i].data.fd;
    if (fd == listen_fd) {
        // Accept new client...
    } else {
        // Read binary OrderRequest and push to engine...
    }
}
```
