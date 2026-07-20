# Memory Layout and Zero-Allocation Architecture

The matching engine never calls the operating system for memory while it is running. The entire memory arena is allocated once at startup, and from that point forward every order allocation and deallocation is handled by an internal free-list living inside that pre-allocated slab. This document explains that architecture and describes the two tools that let you observe it in real time.

## Why Zero-Allocation Matters

When the OS kernel allocates memory on the heap it acquires locks, searches free-list trees, and potentially moves memory pages. In a latency-sensitive matching engine any of these operations can stall the event loop for microseconds. By pre-allocating a fixed arena of one million `OrderNode` slots at startup the engine eliminates every one of those stalls from the hot path permanently.

## The MemoryPool

The engine owns one `MemoryPool` instance for its entire lifetime. The pool is backed by a `std::vector<OrderNode>` of capacity one million. A separate `std::vector<uint32_t>` tracks the free-list: it stores the indices of available slots in a stack-like structure so that both `alloc()` and `free()` run in O(1) time.

```cpp
// memory_pool.hpp (simplified)
class MemoryPool {
    std::vector<OrderNode> pool_;       // One million pre-allocated slots
    std::vector<uint32_t>  free_list_;  // Indices of available slots
    uint32_t               next_free_;  // Index into free_list_ of the next slot to give out

public:
    uint32_t alloc();          // Pop an index from free_list_. O(1).
    void     free(uint32_t);   // Push the index back onto free_list_. O(1).
    OrderNode& get(uint32_t);  // Direct indexed access into pool_. O(1).
};
```

When `new_order()` is called the engine calls `pool_->alloc()` to get a slot index, writes the order fields into `pool_[slot]`, and records the slot index in a flat `order_directory_` vector. When a fill fully consumes an order or a cancel is received, `pool_->free(slot)` is called, and the slot is immediately available for the next incoming order. No OS call is made at any point.

## How the Visualizers Get Their Data

Both the memory visualizer and the node visualizer use the engine's query protocol. The engine is the server. The visualizer is the client. When the visualizer wants a snapshot it sends a standard 14-byte request packet with a specific type byte. The engine intercepts the packet inside the `epoll` loop before the matching code ever sees it, builds the binary response, and sends it back immediately.

```
Memory Visualizer                    C++ Matching Engine
    |                                        |
    |  send 14 bytes, type = 'M'  -------->  |
    |                                        |  [epoll loop reads type == 'M']
    |                                        |  [calls engine.get_memory_snapshot()]
    |                                        |  [packs 49-byte MemoryStateSnapshot]
    |  <------ receive 49 bytes  ----------  |
    |  [renders free list head,              |
    |   active count, slot indices]          |
```

The same flow applies for the node visualizer with type `'D'` and a 289-byte `NodeSnapshot` response.

## Memory Layout Visualizer (`memory_visualizer.py`)

This tool gives you a high-level view of the pool state: how many orders are alive, how many slots are free, and which physical slot indices are currently occupied.

The `next_free_idx` field is the index of the slot that will be handed out on the very next `alloc()` call. As you submit orders you will watch this number climb. As orders fill or are cancelled you will watch it retreat.

```python
REQ_FMT = '<cIIIc'

req = struct.pack(REQ_FMT, b'M', 0, 0, 0, b'B')
socket.sendall(req)

data = recvall(socket, 49)
unpacked = struct.unpack('<cII10I', data)

next_free_idx = unpacked[1]   # Next slot the pool will hand out
total_active  = unpacked[2]   # Number of live orders right now
used_slots    = unpacked[3:13] # First 10 physical slot indices in use
```

**Example output with three resting orders:**

```
Total Arena Capacity:      1,000,000 slots
Active Live Orders:        3 slots
Available Free Slots:      999,997 slots

--- Pointers ---
Free List Head Index:      -> [3]

--- Top Physical Slots in Use (Order Directory) ---
  Slot Index: [0]
  Slot Index: [1]
  Slot Index: [2]
```

## Node Data Visualizer (`node_visualizer.py`)

This tool goes one level deeper. Instead of showing aggregate counts it shows you the full raw struct contents of every active `OrderNode`. You can inspect each node's order id, price, remaining quantity, and side. More importantly, you can see the `prev_idx` and `next_idx` fields that chain orders together at the same price level.

When a price level holds only one order, both `prev_idx` and `next_idx` are `NONE` (the UINT32_MAX sentinel). When a second order arrives at the same price the engine appends it to the tail of the doubly linked list, and both pointers update accordingly.

```cpp
// OrderNode struct (from types.hpp)
struct OrderNode {
    uint32_t order_id;
    uint32_t price;
    uint32_t quantity;
    char     side;
    char     _pad[3];
    uint32_t next_idx;  // UINT32_MAX means this node is the tail
    uint32_t prev_idx;  // UINT32_MAX means this node is the head
};
```

The visualizer unpacks this struct from the binary response and renders a table showing the linked list pointers as slot numbers so you can trace the chain visually.

**Example output with two buy orders at $100.00 and one sell at $101.00:**

```
  Slot  OrdID  Side       Price       Qty   Prev Slot  Next Slot
  ----------------------------------------------------------------
  [  0]      1   BID    $100.00       100        NONE          1
  [  1]      2   BID    $100.00        50           0       NONE
  [  2]      3   ASK    $101.00        30        NONE       NONE

  Linked List Chains at Shared Price Levels:
    $100.00: [slot 0 ord 1] <-> [slot 1 ord 2]
```

Slot 0 has `next_idx = 1` meaning the next node in its price queue is at pool slot 1. Slot 1 has `prev_idx = 0` pointing back. Slot 2 is completely isolated because it is the only order at $101.00. If a new buy order at $100.00 arrived it would appear as slot 3 with `prev_idx = 1` and slot 1's `next_idx` would update to 3.
