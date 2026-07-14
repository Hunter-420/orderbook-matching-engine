# Designing a Single Threaded epoll Matching Engine in C++ for Deterministic Order Execution

**Executive pitch:** How one CPU thread and Linux epoll enforce price and time priority in a C++ limit order book, with zero locks and zero heap allocation on the matching path.

**Summary:** I am building a C++ limit order book matching engine and this article covers the network architecture behind it. It walks through why I chose a single thread built around Linux epoll instead of a thread per client model, using one running example: two buy orders for AAPL 750 nanoseconds apart that must be matched in the exact order they arrived. From there it covers nonblocking sockets, outbound backpressure, fairness between clients, preallocated memory pools instead of heap allocation, a lock free ring buffer for audit logging, how epoll uses a red black tree internally, and where io_uring fits as a future upgrade. The goal throughout is one guarantee: the sequence of network arrival always matches the sequence of matching, with zero locks and zero heap allocation on the hot path.

`#Cpp` `#SystemsProgramming` `#LowLatency` `#TradingSystems` `#LimitOrderBook` `#LinuxEpoll` `#NetworkProgramming` `#HighFrequencyTrading` `#PerformanceEngineering` `#io_uring`

**June 18, 2026**

`C++17` `C++20` `g++` `clang++` `Linux Kernel` `Makefile` `Python` `POSIX Threads (pthread)` `Linux epoll API` `Nonblocking Sockets` `Fixed Width Binary Serialization` `Memory Arenas` `strace` `perf` `TCP_NODELAY (Nagle's Algorithm suppression)`

I am building a C++ matching engine that accepts buy and sell orders from real network clients, stores all order state in a memory pool that is allocated once at startup, matches orders using price and time priority, speaks a fixed width binary protocol over the wire, and measures its own end to end latency at nanosecond resolution. No memory is allocated on the heap while an order is being matched. No two threads ever touch the same piece of data. There is no garbage collector running in the background.

My goal is simple to state and hard to achieve: squeeze the maximum possible performance out of a single physical CPU core. Every operating system call, every lock, every cache miss, and every thread switch adds delay. This project is about removing all of that delay at the source, not patching over it later.

This article covers the part of the project where I chose the network model. It explains, in my own words and from a few different angles, why I picked a single thread built around the Linux `epoll` API instead of the more familiar thread per client or thread pool approach that most web backends use.

**Background:** I wrote an article called [Decoupled Vector-Map Data Layout for Allocation-Free Limit Order Book](https://www.khanalnischal.com.np/writing/decoupled-vector-map-data-layout-for-allocation-free-limit-order-book) before starting this project. That article covers the vector map layout itself: the three layer architecture of a flat [memory pool](https://en.wikipedia.org/wiki/Memory_pool), a sorted price map, and an order directory, which together let an [order book](https://en.wikipedia.org/wiki/Order_book) insert, cancel, and match orders with zero heap allocation. It traces every insertion, every cancellation, and every match scenario with full memory snapshots, but it only covers storage and retrieval inside that layout. This article, [Designing a Single Threaded epoll Matching Engine in C++ for Deterministic Order Execution](https://www.khanalnischal.com.np/writing/single-threaded-epoll-matching-engine-in-c-for-deterministic-order-execution), covers what connects to that vector map layout from the outside: the network that delivers orders, the event loop that drives the whole system, the binary protocol that eliminates parsing overhead, and the hardware isolation that keeps latency predictable. This project takes the vector map order book from the first article and builds the rest of the engine around it.

**Project link:** [Single Threaded Limit Order Book Matching Engine in C++](https://www.khanalnischal.com.np/projects/single-threaded-limit-order-book-matching-engine-in-c)

## The Problem, Stated Plainly

Here is the exact problem this article solves: a limit order book has a strict rule called **price and time priority**, meaning that at a given price, the order that reached the exchange first must be filled first. The order book algorithm itself is simple to get right. The hard part, the part this article is actually about, is guaranteeing that the sequence of orders reaching my matching function is the same sequence in which those orders actually arrived on the network, down to the microsecond, even while I am reading from thousands of client connections at once.

Let me walk through a precise example using my own order book.

Say my book for stock symbol **AAPL** currently has one resting sell order: **Order 100**, sell 200 shares at **189.50 dollars**, placed by **Client A**. Two new buy orders for AAPL at 189.50 dollars arrive over the network within a fraction of a millisecond of each other:

- **Order 101**, buy 200 shares at 189.50 dollars, sent by **Client B**, and the packet lands on the network card at timestamp 09:30:01.000041200.
- **Order 102**, buy 200 shares at 189.50 dollars, sent by **Client C**, and the packet lands on the network card at timestamp 09:30:01.000041950.

Client B is earlier by **750 nanoseconds**. The correct outcome, the only legally correct outcome, is that **Order 101 from Client B is matched in full** against the resting Order 100, and **Order 102 from Client C rests in the book unfilled**, waiting for the next available seller.

Now here is where the network layer, not the matching algorithm, decides whether I get this right. Order 101 and Order 102 arrive on two different client sockets. If my program reads those sockets by looping over a list and checking each one in turn, or by handing each connection to its own operating system thread, the order in which my code actually sees these two packets depends on scheduling details inside the kernel and the operating system, not on the 750 nanosecond gap between them on the wire. A slower socket check, a thread that gets paused a moment longer, or a scan that happens to reach Client C's socket first, can hand Order 102 to my matching function before Order 101, and Client C fills instead of Client B. **The matching code would run correctly. The result would still be wrong.**

This is the actual problem I am solving in this project: I need a way to read from thousands of client sockets where the sequence I hand orders to my matching function is guaranteed to follow the sequence in which those orders actually arrived, with no gap for the operating system to reorder them in between. The rest of this article walks through the architecture I built to guarantee exactly that, starting with why I picked a single thread and the Linux `epoll` interface over the thread per client model most servers use.

## Table of Contents

1. [Why I Rejected Threads and Locks](#1-why-i-rejected-threads-and-locks)
2. [What epoll Actually Does](#2-what-epoll-actually-does)
3. [The Shape of My Event Loop](#3-the-shape-of-my-event-loop)
4. [The Outbound Problem Nobody Talks About](#4-the-outbound-problem-nobody-talks-about)
5. [The Starvation Problem](#5-the-starvation-problem)
6. [Why I Do Not Use new or std::map on the Hot Path](#6-why-i-do-not-use-new-or-stdmap-on-the-hot-path)
7. [Logging Without Slowing Down the Engine](#7-logging-without-slowing-down-the-engine)
8. [What Is Actually Happening Inside the Kernel](#8-what-is-actually-happening-inside-the-kernel)
9. [Why the Kernel Trusts a Red Black Tree](#9-why-the-kernel-trusts-a-red-black-tree)
10. [Where This Goes Next: io_uring](#10-where-this-goes-next-io_uring)
11. [The Tradeoffs I Am Accepting for Now](#11-the-tradeoffs-i-am-accepting-for-now)
12. [Designing the Order Book and the Network Together](#12-designing-the-order-book-and-the-network-together)
13. [Conclusion](#13-conclusion)
14. [References](#14-references)

## 1. Why I Rejected Threads and Locks

Before I settled on a single thread, I looked at this from three different angles: the exchange operator's angle, the CPU's angle, and the regulator's angle. All three pointed at the same answer, and all three explain why the Client B and Client C example above cannot be left to chance.

To make the risk concrete, imagine I had built this engine using a normal multi threaded server design instead, where every client connection gets its own operating system thread, and every thread calls a shared `match_order()` function on one shared order book. Since Thread B and Thread C could call `match_order()` within nanoseconds of each other, I would have to wrap the order book in a [`std::mutex`](https://en.cppreference.com/w/cpp/thread/mutex) to stop them from corrupting it at the same time. Whichever thread reaches that lock first wins, and which thread reaches the lock first is decided by the operating system scheduler based on CPU load, not by which client actually sent their packet first on the wire. That is precisely how Order 102 could jump ahead of Order 101.

**From the exchange operator's angle**, correctness matters more than raw throughput. I would rather process one hundred thousand orders a second correctly than one million orders a second with an occasional wrong fill.

**From the CPU's angle**, locks are expensive. A modern CPU keeps recently used data in small, extremely fast memory caches sitting right next to the processor core, usually called [L1 and L2 cache](https://en.wikipedia.org/wiki/CPU_cache). Reading from these caches takes a few nanoseconds. Reading from main system memory, by comparison, takes around **one hundred nanoseconds**, which sounds small until you realize that is one hundred times slower. When the operating system performs a [context switch](https://en.wikipedia.org/wiki/Context_switch) between which thread is running on a core, it usually has to flush whatever that thread had stored in the cache to make room for the next thread. So even without a single lock ever being contended, just having many threads take turns on one core is enough to slow everything down through repeated cache flushing. This is also why I pin my single matching thread to one specific physical CPU core using [`pthread_setaffinity_np()`](https://man7.org/linux/man-pages/man3/pthread_setaffinity_np.3.html), so the operating system can never move it and force a fresh cache warm up somewhere else.

**From the regulator's angle**, an exchange has to be **deterministic**. If I feed the same one thousand orders into my engine twice, I should get the exact same fills, in the exact same order, both times. That property is what lets an exchange investigate a dispute or replay a trading day for an audit. A multi threaded engine, where a scheduler quietly decides thread order based on CPU load, cannot make that promise. A single thread processing one event at a time can.

Here is a short comparison table that sums up the difference:

| Property | Thread pool with locks | Single thread with epoll |
|---|---|---|
| Locking needed on the order book | Yes, a mutex or spinlock guards shared state | No, only one thread ever touches it |
| CPU cache behavior | Poor, constant switching flushes the cache | Good, the order book stays hot in cache |
| Outcome if run twice with the same input | Not guaranteed to match | Guaranteed to match |
| Number of connections it scales to | Hundreds, before threads become the bottleneck | Thousands, because waiting is handled by the kernel, not by threads |

## 2. What epoll Actually Does

The core idea behind [`epoll`](https://man7.org/linux/man-pages/man7/epoll.7.html) is that my program should never have to ask each client connection, one by one, whether it has new data. Instead, I ask the Linux kernel to watch all of my connections for me, and to hand me a short list containing only the ones that actually have something to say.

Think of it like a call button system in a large restaurant. The old way of doing things, where a program checks each connection in turn using older tools like [`select`](https://man7.org/linux/man-pages/man2/select.2.html) or `poll`, is like a waiter walking to every single table and asking "are you ready to order yet." If the restaurant has one hundred tables and only two of them are ready, the waiter still wasted time walking to all one hundred. As the restaurant grows, the waiter spends all their time walking and none of their time actually taking orders.

The `epoll` way gives every table a call button. The waiter sits at the counter, which is the equivalent of calling `epoll_wait()`, and only walks toward a table when its button lights up. If nobody has pressed a button, the waiter just waits there, using no extra effort at all.

Here is what that looks like on my exchange in practice. Say I have **5000 clients connected** at once, but at this particular instant only three of them have sent anything: Client B's socket has Order 101 sitting in it, Client C's socket has Order 102 sitting in it, and a market data subscriber, Client D, has just sent a heartbeat message. With the old `select` style approach, my program would have to check the status of all 5000 sockets, one at a time, just to find those three. With `epoll`, I call `epoll_wait()` once, and the kernel hands back a list containing exactly three file descriptors, the ones for Client B, Client C, and Client D, and nothing else. I never touch the other 4997 sockets at all.

There are three function calls that make this work:

- [`epoll_create1()`](https://man7.org/linux/man-pages/man2/epoll_create1.2.html) asks the kernel to set up a new watch list and hands back a number, called a [file descriptor](https://en.wikipedia.org/wiki/File_descriptor), that represents that list.
- [`epoll_ctl()`](https://man7.org/linux/man-pages/man2/epoll_ctl.2.html) adds or removes a specific client connection from that watch list, and tells the kernel what kind of activity to watch for, such as data becoming available to read.
- [`epoll_wait()`](https://man7.org/linux/man-pages/man2/epoll_wait.2.html) is the call that actually pauses my thread until the kernel has something to report, then hands back a short list of exactly which connections are ready.

The important part is that my program never has to guess or poll blindly. The kernel does the watching, and my single thread only wakes up to do real work.

## 3. The Shape of My Event Loop

Once I understood the three calls above, the shape of the engine became a loop that never stops running for the lifetime of the process. Here is a simplified version of what that loop looks like in my code:

```cpp
#include <sys/epoll.h>
#include <vector>

const int MAX_EVENTS = 64;

void run_engine(int epoll_fd, int listen_socket, OrderBook& order_book) {
    std::vector<epoll_event> events(MAX_EVENTS);

    while (true) {
        int num_fds = epoll_wait(epoll_fd, events.data(), MAX_EVENTS, -1);

        for (int i = 0; i < num_fds; ++i) {
            int current_fd = events[i].data.fd;

            if (current_fd == listen_socket) {
                int client_fd = accept_connection(listen_socket);
                set_nonblocking(client_fd);

                epoll_event ev{};
                ev.events = EPOLLIN | EPOLLET;
                ev.data.fd = client_fd;
                epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev);
            } else {
                char buffer[1024];
                ssize_t bytes_read = recv(current_fd, buffer, sizeof(buffer), 0);

                if (bytes_read > 0) {
                    Order order = parse_order(buffer, bytes_read);
                    order_book.process_order(order);
                } else if (bytes_read == 0 || (bytes_read < 0 && errno != EAGAIN)) {
                    close(current_fd);
                }
            }
        }
    }
}
```

Every socket I hand to the kernel is set to nonblocking mode first, using [`fcntl`](https://man7.org/linux/man-pages/man2/fcntl.2.html) with the `O_NONBLOCK` flag. That step matters a lot. In a normal blocking socket, calling [`recv()`](https://man7.org/linux/man-pages/man2/recv.2.html) pauses the whole thread until data shows up, which would freeze my entire exchange while waiting on a single slow client. With a nonblocking socket, `recv()` returns immediately, either with the data that is available or with an error code called [`EAGAIN`](https://man7.org/linux/man-pages/man3/errno.3.html) that just means "nothing here right now, try later."

I also chose [edge triggered mode](https://man7.org/linux/man-pages/man7/epoll.7.html), set with the `EPOLLET` flag, over the default level triggered mode. In level triggered mode, the kernel keeps reminding me that a socket has unread data every time I call `epoll_wait()`, for as long as any data remains in the buffer. In edge triggered mode, the kernel tells me exactly once, the moment new data shows up. That is slightly faster because it means fewer trips into the kernel, but it comes with a rule I have to follow carefully: I must keep calling `recv()` in a loop until it returns `EAGAIN`, or I risk missing data that arrived after the one notification I got.

Tracing my Order 101 example through this exact loop makes the mechanics concrete. `epoll_wait()` returns and tells me Client B's file descriptor is ready. My code calls `recv()` on that socket and gets back **24 raw bytes**, the fixed width binary encoding of Order 101, buy 200 shares of AAPL at 189.50 dollars. `parse_order()` casts those 24 bytes directly onto my `Order` struct, with no string parsing involved. `order_book.process_order()` is called immediately, on the same pass through the loop, and it finds the resting Order 100 from Client A and matches the two. By the time `epoll_wait()` is called again, **Order 101 has already been fully processed**, well before Order 102 from Client C is even read off its socket.

## 4. The Outbound Problem Nobody Talks About

Most articles about `epoll` focus entirely on reading incoming orders. I ran into a much less obvious problem on the way out: sending data back to clients can freeze the engine just as badly as a slow client sending data in.

Here is the scenario. My engine matches a trade and needs to send an execution report back to the client. If that client has a slow or congested internet connection, the operating system's outbound buffer for that socket fills up. If I call a normal blocking `send()` at that point, my single thread stalls, waiting for that one client's network connection to clear up, while every other client on the exchange, including ones with excellent connections, sits frozen and unable to trade.

Continuing my running example: after matching Order 101 against Order 100, I owe an execution report to Client A and Client B, and I owe a public market data update to every other subscribed client, including Client D. Say Client D is on a congested mobile connection, and their operating system's receive buffer, typically around **64 kilobytes**, is already full from an earlier burst of updates. A blocking `send()` to Client D would freeze my thread right there, meaning Client A and Client B, both on fast fiber connections, would not get their execution reports until Client D's connection finally clears up, even though neither of them did anything wrong.

The fix mirrors what I already do for reading. I make outbound sockets nonblocking too, and I set [`TCP_NODELAY`](https://en.wikipedia.org/wiki/Nagle%27s_algorithm) on every client socket to disable Nagle's algorithm, which otherwise delays small outgoing packets in an attempt to bundle them together, an optimization built for typical web traffic that only adds unwanted delay to a tiny execution report. When `send()` to Client D returns `EAGAIN`, instead of waiting, I copy the unsent bytes into a small queue I keep just for Client D in my own memory, register interest in the `EPOLLOUT` event for that socket, and immediately move on to sending Client A and Client B their reports without any further delay. When the kernel later tells me Client D's socket is writable again, I flush whatever is left in that queue. **Client D's slow connection can no longer hold the rest of the exchange hostage.**

## 5. The Starvation Problem

Edge triggered mode brought a second, less obvious risk: fairness between clients. If I use edge triggered mode correctly, I have to drain a socket completely by calling `recv()` in a loop until it returns `EAGAIN`. But imagine one client, maybe a high frequency trading firm called **Client E**, starts flooding my engine with **thousands of AAPL orders per second**. My thread could get stuck reading from Client E's socket for a long stretch of time, while a completely ordinary order from another client, **Client F**, sits unread and unprocessed in the kernel's socket buffer, quietly building up latency even though Client F only sent one order.

The fix I use is a simple quota. On each pass through the loop, I read at most a fixed number of packets, say **sixteen**, or a fixed number of bytes, from any single client socket. So when Client E's socket is drained, I stop after sixteen orders, remember that Client E still has more waiting, and move on to check Client F's socket before coming back to Client E. **This keeps Client E's high volume of traffic from starving Client F of processing time.**

## 6. Why I Do Not Use new or std::map on the Hot Path

The order book is the part of the system that runs on every single order, so it has to be fast in a way that is measured in nanoseconds, not milliseconds. That rules out a lot of ordinary, comfortable C++.

If I called `new` to create every incoming `Order` object, or used a `std::shared_ptr` to manage its lifetime, the default memory allocator on Linux would step in. That allocator uses internal locks to stay safe when multiple threads are involved, and even though my engine only has one thread doing the matching, that allocator can still be forced to coordinate with other background threads in the process, adding delay for no benefit. On top of that, memory handed out by `new` tends to be scattered across RAM rather than sitting next to related data, which causes exactly the kind of CPU cache misses I already worked hard to avoid by using a single thread in the first place.

My fix is to preallocate everything before the trading session even starts. I create a fixed size pool of, say, **one million `Order` slots** as a plain array at startup. When Order 103 arrives from another client, I do not call `new`. Instead, I take the next free index out of that array, for example **slot number 47281**, write the order's fields directly into it, and hand back a plain integer index instead of a pointer. When Order 103 is fully filled or cancelled, I return index 47281 to the free list so it can be reused by the next incoming order. That single array lookup and write takes **a few nanoseconds**, versus the roughly **one hundred nanoseconds** a heap allocation through `new` can cost once lock contention and memory fragmentation are accounted for. The same logic applies to the order book's internal structures: I keep the active price levels in flat, contiguous arrays rather than in pointer heavy structures like `std::map`, so that when I walk through price levels to match a large order, the data I need is already sitting close together in the CPU cache instead of scattered across memory.

## 7. Logging Without Slowing Down the Engine

An exchange has to keep a permanent audit log of every order and every trade, and it has to broadcast public market data updates to everyone watching the market. Both of these involve writing to disk or sending data over the network, and both are far too slow to do directly inside the matching loop.

I solve this the same way trading firms solve it: I hand the slow work off to a second thread, but I do it without a single lock. I use a data structure called a [single producer single consumer ring buffer](https://www.boost.org/doc/libs/release/doc/html/boost/lockfree/spsc_queue.html), often shortened to SPSC ring buffer. My matching thread is the only producer, and a background logging thread on a separate CPU core is the only consumer.

A useful way to picture this is a sushi conveyor belt in a restaurant kitchen. My matching thread is the chef. He plates a tiny trade record and sets it down on the belt, then immediately goes back to preparing the next order, never once stopping to talk to anyone. The logging thread is the waiter standing at the far end of the belt, picking up plates as they arrive and carrying them out to be written to disk. If the waiter falls behind, the belt simply fills up a little, but the chef and the waiter never reach for the same plate at the same time, so there is nothing to corrupt and nothing to lock.

Following my running example, the moment Order 101 fully fills Order 100, my matching thread builds a small `TradeLog` record, roughly **16 bytes** containing the order id, quantity, and price, and calls `push()` on the ring buffer. That call places the record into **slot 0 of a 1024 slot buffer** and moves the tail pointer to slot 1, all in a few nanoseconds, before my matching thread immediately moves on to reading Order 102. On a separate CPU core, the logging thread calls `pop()`, reads the record out of slot 0, moves the head pointer to slot 1, and writes it to the audit log file on disk. **My matching thread never once waits to find out whether that disk write has actually finished.**

Here is a simplified version of that ring buffer:

```cpp
#include <atomic>
#include <vector>

template <typename T, size_t Capacity>
class SPSCRingBuffer {
private:
    std::vector<T> buffer_;
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};

public:
    SPSCRingBuffer() : buffer_(Capacity) {}

    // Called only by the matching engine thread
    bool push(const T& item) {
        size_t current_tail = tail_.load(std::memory_order_relaxed);
        size_t current_head = head_.load(std::memory_order_acquire);

        if ((current_tail + 1) % Capacity == current_head) {
            return false; // buffer is full
        }

        buffer_[current_tail] = item;
        tail_.store((current_tail + 1) % Capacity, std::memory_order_release);
        return true;
    }

    // Called only by the background logging thread
    bool pop(T& value) {
        size_t current_head = head_.load(std::memory_order_relaxed);
        size_t current_tail = tail_.load(std::memory_order_acquire);

        if (current_head == current_tail) {
            return false; // buffer is empty
        }

        value = buffer_[current_head];
        head_.store((current_head + 1) % Capacity, std::memory_order_release);
        return true;
    }
};
```

The reason this stays safe without a mutex is that the two threads never write to the same variable. The matching thread only ever moves the tail pointer forward. The logging thread only ever moves the head pointer forward. They read each other's pointer using [atomic operations and a defined memory order](https://en.cppreference.com/w/cpp/atomic/memory_order.html), so the CPU guarantees the update is visible instantly and without corruption, but they never fight over the same piece of memory.

## 8. What Is Actually Happening Inside the Kernel

It helps to know what happens on the other side of the wall, inside the Linux kernel itself, when I call these functions.

When I register a client socket with `epoll_ctl()`, the kernel stores that socket inside an internal data structure called a [red black tree](https://en.wikipedia.org/wiki/Red%E2%80%93black_tree), which I will get to in the next section. When a network packet actually arrives for one of my client sockets, the network card and the kernel's network stack place that socket onto a separate list called the ready list. When I call `epoll_wait()`, the kernel does not search anything. It simply looks at the ready list, and if it is empty, my thread goes to sleep efficiently, using no CPU at all, until something shows up.

Back to my 5000 client example: Client B's and Client C's sockets already sit inside that red black tree, placed there once each when they first connected, using `EPOLL_CTL_ADD`. The moment Order 101 lands on Client B's socket, **the kernel does not walk the tree at all**. It simply drops Client B's file descriptor onto the ready list. When my thread calls `epoll_wait()` a moment later, it reads that ready list directly, finds Client B waiting there, and **never has to search through the other 4999 entries** in the tree to find it.

This matters because it means the cost of `epoll_wait()` does not grow as I add more clients. Whether I have ten clients connected or one hundred thousand, checking the ready list takes roughly the same tiny amount of time.

## 9. Why the Kernel Trusts a Red Black Tree

A regular binary search tree can quietly turn into a disaster. If I insert values into it in already sorted order, it stops looking like a tree and starts looking like a straight line, essentially becoming a slow linked list. Looking something up in that broken tree takes a number of steps equal to the number of items in it, instead of the small number of steps a balanced tree would take.

A red black tree is a self balancing binary search tree that refuses to let this happen. It gives every node an extra bit of information, a color that is either red or black, and enforces five rules while inserting or deleting nodes, including that the root is always black and that a red node can never have a red child. Whenever an insertion or deletion would break these rules, the tree quickly repairs itself using operations called rotations and color flips.

The reason this matters to `epoll` is that the kernel uses exactly this structure to keep track of every socket registered to a watch list. Whether my exchange has ten clients or one hundred thousand clients, adding, removing, or looking up a socket inside that tree takes a small and predictable number of steps, roughly the logarithm of the number of items, rather than growing in proportion to the total number of clients. To put a real number on it, looking up one specific socket among one hundred thousand connected clients takes around **seventeen comparisons** in a red black tree, compared to **up to one hundred thousand comparisons** if the kernel had used a plain unsorted list instead. That predictability is worth more to an exchange than raw speed on paper, because a system that occasionally takes far longer than usual is worse than a system that is consistently fast.

It is worth noting that I deliberately do not use this same structure, or its C++ equivalent `std::map`, inside my actual order book. A tree like this uses pointers scattered across memory, which is exactly what I am trying to avoid on my hot path. It is the right tool for the kernel to manage a constantly changing list of sockets, but the wrong tool for matching orders where every nanosecond and every cache line counts.

## 10. Where This Goes Next: io_uring

Right now my engine uses `epoll`, and every time a packet arrives, my thread makes two separate trips into the kernel: one to find out that data is ready, through `epoll_wait()`, and a second one to actually copy that data into my program's memory, through `recv()`. Each of those trips is called a system call, and each one forces the CPU to cross a boundary between the restricted kernel space and my ordinary program's user space. That crossing, called a context switch, costs somewhere in the range of a few hundred nanoseconds, mostly because it clears out the CPU's predictions about what code is coming next.

A newer part of the Linux kernel called [`io_uring`](https://man7.org/linux/man-pages/man7/io_uring.7.html) removes almost all of that cost. Instead of asking the kernel a question and then asking it again to get an answer, I set up two ring buffers that live in memory shared directly between my program and the kernel. I place a request onto the submission queue, for example "read from this client into this exact block of memory," and I never make a system call to place it. The kernel, watching that same shared memory, does the work in the background and places a completion entry onto a second buffer called the completion queue when it is done. My program simply looks at the completion queue and finds the data is already sitting in the exact block of memory I asked for, with no `recv()` call needed at all.

A restaurant analogy again helps make the difference clear. `epoll` is reactive: a customer presses a buzzer, and I walk to their table to write down their order myself. `io_uring` is proactive: I leave a blank order pad on every table in advance, and I tell every customer that the moment they finish writing, a food runner will grab the paper and drop it directly onto my kitchen counter. I never leave the kitchen. I just watch completed slips appear under the heat lamp, already filled out, the instant I look up.

Applying this to Order 101 makes the difference concrete. With `epoll`, my thread learns Client B's socket is ready, and then still has to execute `recv()` as a separate system call to actually copy those 24 bytes out of the kernel. With `io_uring`, I would have already submitted a read request for Client B's socket ahead of time, pointing at a specific block of memory. The moment Order 101 arrives, the kernel copies it directly into that memory block on its own. When my loop checks the completion queue, **the 24 bytes of Order 101 are already sitting there, fully formed**, and I call `order_book.process_order()` immediately with no `recv()` call in between.

I am not moving to `io_uring` yet. I am staying with `epoll` for now because it is simpler to write correctly, easier to debug, and already fast enough that my current bottleneck is inside my own matching logic, not inside the network layer. I plan to revisit this once I have profiled the engine with a tool like `perf` and confirmed that a meaningful share of CPU time, something like thirty to forty percent, is being spent inside `epoll_wait()` and `recv()` rather than inside actual order matching.

## 11. The Tradeoffs I Am Accepting for Now

I want to be honest about the tradeoffs involved in eventually switching to `io_uring`, because none of this is a free upgrade.

The first tradeoff is code complexity. With `epoll`, my code reads top to bottom in a way that matches how the event actually happens: data arrives, I call `recv()` on the very next line, and I process it immediately. With `io_uring`, I have to submit a request, store a small block of memory that remembers what that request was for, hand control back to the kernel, and only reconnect with that memory block later when a completion entry shows up, possibly after many other unrelated events have already been processed. Something as simple as **Client D disconnecting midway through five queued execution reports** now requires me to carefully track which of those five outstanding writes have already completed before I am allowed to free Client D's memory block, or I risk a serious bug where the kernel writes into memory I have already released.

The second tradeoff is unpredictability at the extreme tail of my latency numbers. `io_uring` has a mode called [`IORING_SETUP_SQPOLL`](https://man7.org/linux/man-pages/man7/io_uring.7.html) where the kernel spins up its own background thread to continuously poll my submission queue, removing system calls almost entirely. That is extremely fast on average, but if that kernel thread happens to share physical CPU resources with my own matching thread, for example through [hyperthreading](https://en.wikipedia.org/wiki/Hyper-threading), it can introduce small, unpredictable pauses. My average latency would very likely drop, but my worst case latency during a burst of traffic could become less predictable, which matters more to an exchange than the average does.

The third tradeoff is portability. `epoll` works on essentially every Linux machine built in the last fifteen years. `io_uring` needs a modern kernel, generally version 5.10 or newer, with the best performance features only fully available on the 6.x kernel series. If this engine ever needs to run on an older corporate server, `io_uring` might not even be available, while `epoll` would work without any changes at all.

## 12. Designing the Order Book and the Network Together

The last lesson I learned while building this is that the order book and the network layer cannot be designed separately and bolted together afterward. If I design the matching logic assuming a perfectly formed `Order` object magically appears in memory, I will end up with an engine that looks blazing fast in an artificial benchmark but falls apart the moment it talks to real clients over a real network.

Here are four concrete ways this shows up, from four different angles.

**From the protocol angle:** a clean C++ `Order` struct with fixed integer fields can be processed in a few nanoseconds. Real clients, however, send raw bytes over the wire using protocols like [FIX](https://www.fixtrading.org/) or JSON. If my matching thread has to convert incoming text into integers using something like `std::stoi` every time an order arrives, my few nanoseconds of matching time can balloon into hundreds of nanoseconds of string parsing. My fix is to design my wire protocol as a **fixed width binary layout** that matches my internal struct exactly, so I can cast the raw network bytes directly onto my order structure with no parsing step at all, exactly as I did with the 24 bytes of Order 101 earlier in this article.

**From the market volatility angle:** real networks do not deliver traffic evenly. When a piece of market moving news breaks, dozens of trading firms can blast orders at my server within the same millisecond, a pattern often called a **microburst**. If my network layer is not tuned to drain the kernel's socket buffers quickly during a burst like this, the operating system will start dropping packets, forcing the client's TCP stack to retransmit them, which can add **two hundred milliseconds or more** of delay for that client, an eternity for a system built around nanoseconds.

**From the slow client angle**, which I already covered above with Client D and outbound backpressure, one client with a poor connection should never be able to freeze trading for every other client on the exchange.

**From the memory layout angle:** the buffers `epoll` or `io_uring` use to hold incoming network packets are competing for the exact same CPU cache space as my order book's price levels. For AAPL, I keep the price levels from **189.00 to 190.00 dollars**, in one cent increments, as a flat array of **100 slots**, where the slot for 189.50 dollars is simply **index 50**. If my network buffers and this price level array are scattered across unrelated regions of memory, every incoming packet risks evicting that array from the cache, forcing a slow trip to main memory right when I need to look up index 50 to match Order 101.

The rule I keep coming back to is simple: a fast matching engine with a slow doorway is still a slow engine. Building around a single thread and `epoll` from the very beginning forces me to think about network flow, nonblocking behavior, and memory layout at the same time I am designing my matching logic, instead of treating the network as an afterthought I can optimize later.

## 13. Conclusion

Choosing a single thread built around `epoll` was not about avoiding complexity. It was about moving the complexity to the place where I can actually control it, inside my own code, instead of leaving it to an operating system scheduler that has no idea that price and time priority is a legal requirement, not a suggestion. Every design decision in this article, from nonblocking sockets, to outbound queues for clients like Client D, to fairness quotas against clients like Client E, to preallocated memory pools, to the SPSC ring buffer for logging, exists to protect one property: **that Order 101 from Client B is always matched before Order 102 from Client C**, every single time this scenario plays out, on a single CPU core, with nothing random in between. That property is the actual product I am building. **Raw speed is only valuable once that property is guaranteed.**

## 14. References

- [epoll(7) Linux manual page, man7.org](https://man7.org/linux/man-pages/man7/epoll.7.html)
- [epoll_wait(2) Linux manual page, man7.org](https://man7.org/linux/man-pages/man2/epoll_wait.2.html)
- [fcntl(2) Linux manual page, man7.org](https://man7.org/linux/man-pages/man2/fcntl.2.html)
- [recv(2) Linux manual page, man7.org](https://man7.org/linux/man-pages/man2/recv.2.html)
- [io_uring, the Linux kernel asynchronous I/O interface, kernel.dk](https://kernel.dk/io_uring.pdf)
- [liburing, the official user space library for io_uring, GitHub](https://github.com/axboe/liburing)
- [io_uring(7) Linux manual page, man7.org](https://man7.org/linux/man-pages/man7/io_uring.7.html)
- [Red black tree, Wikipedia](https://en.wikipedia.org/wiki/Red%E2%80%93black_tree)
- [std::atomic and memory order, cppreference.com](https://en.cppreference.com/w/cpp/atomic/memory_order.html)
- [pthread_setaffinity_np(3), Linux manual page, man7.org](https://man7.org/linux/man-pages/man3/pthread_setaffinity_np.3.html)
- [TCP_NODELAY and Nagle's algorithm, Wikipedia](https://en.wikipedia.org/wiki/Nagle%27s_algorithm)
- [FIX Trading Community, official protocol documentation](https://www.fixtrading.org/)
- [boost::lockfree::spsc_queue documentation, Boost C++ Libraries](https://www.boost.org/doc/libs/release/doc/html/boost/lockfree/spsc_queue.html)
