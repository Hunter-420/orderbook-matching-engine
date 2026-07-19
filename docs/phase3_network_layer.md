# Phase 3: Network Layer

The goal of Phase 3 is **connectivity**, bringing the engine online so it can receive orders over a network. We must do this without introducing context-switching overhead or slow text parsing.

## Design

We use a single-threaded architecture using `epoll`. 

Multi-threading the order book is an anti-pattern for ultra-low latency because:
1. Two threads trying to match orders simultaneously require locking (mutexes), which stalls the CPU.
2. The OS scheduler context-switches threads unpredictably, destroying deterministic latency.

By using `epoll` in a single thread, we process the network queues and the matching engine sequentially, blindingly fast, without ever locking.

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
