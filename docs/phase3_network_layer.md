# Phase 3: Network Layer

## What This Phase Does

Phase 3 takes the engine from a standalone in-memory program to a real networked server. It listens on TCP port 9000, accepts multiple client connections simultaneously, and exchanges binary messages over the wire. It does all of this without adding any threads, any locks, or any text parsing overhead.

## The Problem With Naive Networking

The simplest approach to handling multiple clients would be one thread per client. When a buy order from Client A and a sell order from Client B arrive at the same moment, two threads would both try to touch the order book at the same time. To prevent corruption you would need a mutex. A mutex means one thread waiting while the other works. That waiting is measured in microseconds, which is catastrophic for a matching engine.

A second problem would be the message format. If you use JSON you need to scan every byte looking for quote characters, colons, and brackets. You need to convert ASCII digit strings into integers. Even a highly optimised JSON parser takes hundreds of nanoseconds per message.

The network layer in this engine avoids both problems completely.

## The Solution: One Thread, epoll, Binary Protocol

Instead of one thread per client, I use a single thread and ask the Linux kernel to report exactly which sockets have data ready to read at any moment. This is what `epoll` does.

I call `epoll_wait`. This call blocks until at least one socket has incoming data. When it returns, it hands back a list of file descriptors that are ready. I process each one sequentially: read 14 bytes, route to the matching logic, send the execution report back. Then call `epoll_wait` again.

Because everything happens on one thread, the order book is never accessed by two operations simultaneously. No mutex is needed at any point.

For the message format, every packet is a fixed-width binary struct. I know every message is exactly 14 bytes. There is no scanning, no parsing, and no allocation. Reading a message is a single `recv()` call followed by a `memcpy` into a struct.

## Two Clients, One Core

Imagine two clients connect and each sends a 14-byte order.

`epoll_wait` wakes and returns two ready file descriptors.

I read 14 bytes from Client A's socket. I copy those bytes into an `OrderRequest` struct. Since the struct fields are laid out exactly as the bytes arrived, reading the `price` field is a direct memory access with no conversion needed. I call `new_order()` with those values and send back a 14-byte `ExecutionReport` with status `'A'` for accepted.

I then read 14 bytes from Client B's socket and do the same. If Client B's order crosses Client A's price, I generate a fill and send `ExecutionReport` with status `'F'` to both sockets.

Everything runs sequentially on one core. There is no parallelism and no synchronisation.

## The Binary Wire Protocol

The protocol uses `#pragma pack(push, 1)` to tell the C++ compiler to lay out struct fields with no gaps between them. Without this directive the compiler would add invisible padding bytes to align fields on CPU-friendly boundaries, which would change the struct's size and make it impossible for the Python client to predict where each field starts.

```cpp
// include/protocol.hpp
#pragma pack(push, 1)  // No padding allowed

struct OrderRequest {
    char     type;      // 1 byte: 'N' = new, 'C' = cancel, 'O'/'M'/'D' = query
    uint32_t order_id;  // 4 bytes
    uint32_t price;     // 4 bytes, integer cents ($101.00 = 10100)
    uint32_t quantity;  // 4 bytes
    char     side;      // 1 byte: 'B' = buy, 'S' = sell
};                      // Total: 14 bytes, always, on every platform

struct ExecutionReport {
    char     type;       // 1 byte: 'E'
    uint32_t order_id;   // 4 bytes
    uint32_t filled_qty; // 4 bytes
    uint32_t fill_price; // 4 bytes
    char     status;     // 1 byte: 'A' = accepted, 'F' = filled, 'C' = cancelled
};                       // Total: 14 bytes

#pragma pack(pop)

// These lines make the build fail immediately if the sizes are ever wrong
static_assert(sizeof(OrderRequest)   == 14);
static_assert(sizeof(ExecutionReport) == 14);
```

On the Python side, the `struct` module packs and unpacks bytes using a format string. `'<cIIIc'` means little-endian, one char, three unsigned 32-bit integers, one char. That produces exactly 14 bytes.

```python
# tests/manual_client.py
import struct

REQ_FMT = '<cIIIc'  # little-endian: char, uint32, uint32, uint32, char = 14 bytes

# Sending a buy order for 100 shares at $101.00 (price = 10100 cents)
req = struct.pack(REQ_FMT, b'N', 1, 10100, 100, b'B')
socket.sendall(req)  # Sends exactly 14 bytes

# Reading back a 14-byte execution report
data = socket.recv(14)
msg_type, order_id, filled_qty, fill_price, status = struct.unpack(REQ_FMT, data)
```

## Why TCP_NODELAY Is Critical

TCP has a feature called Nagle's Algorithm. By default it batches small packets together before sending them, waiting up to 40 milliseconds in hopes that more data will arrive to send in the same segment. This optimises bandwidth but destroys latency. A 14-byte order packet could sit in the kernel's send buffer for 40 milliseconds before being transmitted.

Setting `TCP_NODELAY` disables Nagle's Algorithm. Every call to `send()` results in an immediate network transmission with no waiting.

```cpp
// src/main.cpp
void set_nodelay(int fd) {
    int opt = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
}
```

I call this on every socket as soon as it is accepted, including the listening socket.

## Why SIGPIPE Is Ignored

When a client disconnects unexpectedly and I call `send()` to write to that now-dead socket, the OS sends a `SIGPIPE` signal to the process. The default handler for `SIGPIPE` terminates the process immediately. One disconnected client would kill the entire exchange.

Setting `SIGPIPE` to `SIG_IGN` makes `send()` return `-1` instead of crashing the process. I see the error return value, close the file descriptor, and continue serving the remaining clients.

```cpp
// src/main.cpp — at the top of main(), before anything else
std::signal(SIGPIPE, SIG_IGN);
```

## The epoll Event Loop

The loop is the heart of the network layer. Every client socket is registered with `epoll_ctl`. When `epoll_wait` returns, it provides a list of only the ready sockets, not all sockets. This keeps the per-iteration work proportional to activity, not to total connection count.

```cpp
// src/main.cpp — main event loop
int n = epoll_wait(epfd, events, MAX_EVENTS, 100 /* ms timeout */);

for (int i = 0; i < n; ++i) {
    int fd = events[i].data.fd;

    if (fd == listen_fd) {
        // A new client is connecting
        int client_fd = accept(listen_fd, ...);
        set_nonblocking(client_fd);
        set_nodelay(client_fd);
        epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &ev);

    } else {
        // An existing client sent data
        char buf[sizeof(OrderRequest)];
        ssize_t bytes = recv(fd, buf, sizeof(buf), 0);

        if (bytes <= 0) {
            // Client disconnected
            close(fd);
        } else if (bytes == sizeof(OrderRequest)) {
            // Process the order
            OrderRequest req;
            std::memcpy(&req, buf, sizeof(OrderRequest));
            // ... route to engine ...
        }
    }
}
```

## Why These Specific C++ and Linux API Choices

**`epoll_wait` instead of `select` or `poll`**

`select` and `poll` are the older Linux APIs for monitoring multiple file descriptors. Both have a fundamental problem: every call requires passing in the entire list of sockets to watch. If I have 500 connected clients, `select` and `poll` must scan all 500 on every single wakeup, even if only one has data. The cost scales as O(N) with the number of connections.

`epoll_wait` works differently. I register each socket with `epoll_ctl` once. After that, `epoll_wait` only returns the file descriptors that are actually ready. If 500 clients are connected but only 2 have sent data, `epoll_wait` returns an array of 2 entries. The work is O(ready events), not O(total connections). This is what lets the engine scale to thousands of connections on one thread.

```cpp
// poll() — O(N) scan every wakeup: must pass all fds every time
int ready = poll(fds, nfds, timeout);  // nfds checked every time

// epoll — O(ready): only the ready fds come back
int n = epoll_wait(epfd, events, MAX_EVENTS, 100);
for (int i = 0; i < n; ++i) {
    int fd = events[i].data.fd;  // Only ready fds, no scanning
    // process fd
}
```

**`recv()` for socket reads**

`recv(fd, buf, sizeof(buf), 0)` reads bytes from the socket into a local buffer. I check the return value: a return of `0` means the client disconnected cleanly, a return of `-1` means an error (I check `errno` and handle `EAGAIN` for non-blocking reads), and a return equal to `sizeof(OrderRequest)` means a complete packet arrived. I only process the message when all 14 bytes are present, avoiding any partial-read logic.

**`std::memcpy` instead of a cast**

After `recv` fills `buf`, I do `std::memcpy(&req, buf, sizeof(OrderRequest))` rather than casting: `OrderRequest* req = (OrderRequest*)buf`. A direct cast would be technically undefined behaviour in C++ if `buf` is not properly aligned. `memcpy` is always safe and modern compilers optimise it to a sequence of load instructions that is identical in generated code to a direct copy.

```cpp
char buf[sizeof(OrderRequest)];
recv(fd, buf, sizeof(buf), 0);

// Safe: memcpy handles alignment
OrderRequest req;
std::memcpy(&req, buf, sizeof(OrderRequest));

// Unsafe cast (undefined behaviour if buf is misaligned):
// OrderRequest* req = reinterpret_cast<OrderRequest*>(buf);
```

**`send()` and checking its return value**

`send(fd, &report, sizeof(report), 0)` writes the 14-byte execution report to the client's TCP socket. With `TCP_NODELAY` set, the kernel sends it immediately. I check the return value: if `send` returns `-1` with `errno == EPIPE`, the client has disconnected. Because I set `SIGPIPE` to `SIG_IGN` earlier, I get the error return instead of a process-killing signal, and I close and remove that file descriptor from epoll.

**`epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev)`**

When a new client connects, I call `accept()` to get their file descriptor and then immediately call `epoll_ctl` with `EPOLL_CTL_ADD` to register it with my epoll instance. From that point on, the kernel will include that file descriptor in future `epoll_wait` results whenever the client sends data. I never have to manually track which clients are active.

## Terminal Output

When the engine starts it announces the port it is listening on. Each client connection and disconnection is logged to stdout. The engine terminal shows the server-side view while the client terminals show the execution reports.
