# Memory Layout and Zero-Allocation Architecture

## Why This Matters

Every time a traditional program creates an object with `new` in C++, it sends a request to the operating system: "give me some memory." The OS finds a free block, marks it as used, and returns a pointer. When the object is destroyed with `delete`, the OS unmarks the block so it can be reused later.

This process is safe and convenient, but it has two serious problems for a matching engine. First, it is slow: a single `new` call can take anywhere from 50 nanoseconds to several hundred microseconds depending on how fragmented the heap is. Second, it is unpredictable: the same call might take 100 nanoseconds one time and 80,000 nanoseconds the next. Predictability matters more than raw speed for a trading engine. Unpredictable delays appear as tail latency spikes in the percentile table.

The matching engine eliminates this problem entirely by pre-allocating all needed memory at startup and never calling the OS again during trading.

## How the MemoryPool Works

At startup the engine creates one `MemoryPool` object. The pool allocates a flat array of exactly one million `OrderNode` slots in a single `new` call. That is the last time `new` is ever called. From that point on, when the engine needs memory for a new order, it takes a slot from this pre-allocated array. When the order is cancelled or fully filled, the slot is returned to the pool.

The pool uses a free-list to track which slots are available. A free-list is a stack of slot indices. At startup the stack contains `[0, 1, 2, ..., 999999]`. When an order arrives, the engine pops an index off the top of the stack and writes the order into that slot. When an order is freed, its index is pushed back onto the top of the stack.

Both operations — pop and push — are a single array access. They are O(1) and never touch the OS heap.

## Walking Through a Live Example

The pool starts with one million slots. The free-list stack top points to slot 0.

A buy order arrives for 100 shares at $100.00. The engine pops slot 0 from the free-list. The free-list top now points to slot 1. The order's data is written into `pool_arena[0]`. The `order_directory_` vector records that order id 1 lives in slot 0.

Another buy order arrives for 50 shares at $100.00. The engine pops slot 1. The free-list top now points to slot 2. The data is written into `pool_arena[1]`. `order_directory_[2] = 1`.

A sell order arrives for 30 shares at $101.00. The engine pops slot 2. The free-list top now points to slot 3. No crossing occurs. The order rests in the ask map. `order_directory_[3] = 2`.

A cancel request arrives for order id 1. The engine looks up `order_directory_[1]`, which says slot 0. It reads `pool_arena[0]` to get the order data, removes the order from the bid map's linked list, marks `order_directory_[1] = INVALID`, and pushes slot 0 back onto the free-list. The free-list top is now 0 again.

At this moment the memory visualizer would show:

```
Active Live Orders:    2 slots (orders 2 and 3 are still resting)
Free List Head:        -> [0]  (slot 0 is available again)
Slots in use:          [1] [2]
```

## How the Visualizers Read This Data

The memory visualizer and node visualizer both connect to the engine as TCP clients. They communicate over the same port 9000 using the same 14-byte binary packet format. When a visualizer wants a snapshot it sends a query packet. The engine intercepts it inside the epoll event loop before the matching code runs, builds the binary response, and sends it back.

This is the complete data path for the memory visualizer:

```
memory_visualizer.py                 C++ Matching Engine
        |                                    |
        | -- 14 bytes, type='M' ----------> |
        |                                    | epoll wakes, reads type byte
        |                                    | type == 'M': calls get_memory_snapshot()
        |                                    | copies next_free_idx, total_active,
        |                                    | and first 10 slot indices into struct
        |                                    | sends 49 bytes back
        | <--- 49 bytes ------------------- |
        | unpacks and renders to terminal    | matching loop continues unaffected
```

The same pattern applies for the node visualizer with type `'D'` and a 289-byte response.

## Memory Visualizer: What Each Field Means

The 49-byte `MemoryStateSnapshot` contains three things.

`next_free_idx` is the pool slot index that `alloc()` will hand out the next time an order arrives. Watching this number across time shows exactly how the free-list advances and retreats as orders are created and destroyed.

`total_active` is the count of orders currently resting in the book. This number goes up when a new order doesn't immediately match and rests. It goes down when an order is fully filled or cancelled.

`top_used_slots` is an array of up to 10 physical slot indices that currently contain live orders. These are the actual array positions in the one-million-element pool arena that are occupied right now.

```python
# How memory_visualizer.py queries the engine
req = struct.pack('<cIIIc', b'M', 0, 0, 0, b'B')  # 14-byte query packet
socket.sendall(req)

data = recvall(socket, 49)  # Always exactly 49 bytes
unpacked = struct.unpack('<cII10I', data)
# unpacked[0] = type byte ('M')
# unpacked[1] = next_free_idx
# unpacked[2] = total_active
# unpacked[3] through unpacked[12] = the 10 slot indices
```

**Example output with two resting orders:**

```
Total Arena Capacity:      1,000,000 slots
Active Live Orders:        2 slots
Available Free Slots:      999,998 slots

Free List Head Index:      -> [2]

Slots Currently in Use:
  Slot Index: [0]
  Slot Index: [1]
```

![Memory visualizer terminal](screenshots/memory_visualizer_output.png)

*(Place a screenshot of the memory pool state display here)*

## Node Visualizer: Seeing the Linked List in Memory

The 289-byte `NodeSnapshot` carries the full raw struct data of up to 10 active `OrderNode` objects. Each entry is 28 bytes and contains every field of the C++ struct exactly as it sits in memory: slot index, order id, price in integer cents, remaining quantity, side, and the `prev_idx` and `next_idx` linked-list pointer fields.

The linked-list pointers are what make this visualizer unique. When a price level holds only one order, both `prev_idx` and `next_idx` are `UINT32_MAX`, which the visualizer displays as `NONE`. When a second order arrives at the same price, it appends to the tail: the new node's `prev_idx` is set to the old tail's slot index, and the old tail's `next_idx` is set to the new node's slot index.

```cpp
// The C++ struct that gets copied into NodeData for the response
struct OrderNode {
    uint32_t order_id;   // Application-level identifier
    uint32_t price;      // Integer cents
    uint32_t quantity;   // Remaining shares
    char     side;       // 'B' or 'S'
    char     _pad[3];    // Alignment padding
    uint32_t next_idx;   // Pool slot of the next order at this price, UINT32_MAX if none
    uint32_t prev_idx;   // Pool slot of the previous order at this price, UINT32_MAX if none
};
```

```python
# How node_visualizer.py queries the engine
NODE_FMT = '<IIIIc3sII'  # 28 bytes: slot(4) oid(4) price(4) qty(4) side(1) pad(3) next(4) prev(4)

req = struct.pack('<cIIIc', b'D', 0, 0, 0, b'B')  # 14-byte query
socket.sendall(req)

data = recvall(socket, 289)  # Always exactly 289 bytes
_, total_active, num_nodes = struct.unpack_from('<cII', data, 0)

for i in range(num_nodes):
    slot, oid, price, qty, side_b, _, next_idx, prev_idx = struct.unpack_from(NODE_FMT, data, 9 + i * 28)
```

**Example: two buys at the same price form a linked list**

You typed `buy 100 100.00` (order 1) and then `buy 50 100.00` (order 2) in the manual client.

```
  Slot  OrdID  Side      Price       Qty   Prev Slot  Next Slot
  [  0]     1   BID   $100.00       100        NONE          1
  [  1]     2   BID   $100.00        50           0       NONE

  Linked List at $100.00:
    [slot 0 ord 1] <-> [slot 1 ord 2]
```

Slot 0 is the head of the price level. Its `next_idx = 1` means the node behind it in the queue is in pool slot 1. Slot 1's `prev_idx = 0` confirms the chain back. If a sell order arrives at $100.00 for 120 shares, the engine will first fill 100 shares from slot 0 (clearing it), then fill 20 shares from slot 1 (leaving 30 remaining).

After those fills you would see:

```
  [  1]     2   BID   $100.00        30        NONE       NONE
```

Slot 0 is gone. Slot 1 is now the only order at that price, so both its `prev_idx` and `next_idx` are back to `NONE`.

![Node visualizer terminal](screenshots/node_visualizer_output.png)

*(Place a screenshot of the live node data display with linked list chains visible here)*
