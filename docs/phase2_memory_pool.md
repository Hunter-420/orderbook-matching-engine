# Phase 2: Memory Pool Optimization

## What This Phase Does

Phase 2 makes the engine fast in a very specific way. I eliminate every operating system memory allocation from the critical trading path. When Phase 1 creates a new order it calls `new OrderNode()`, which asks the OS for memory. When Phase 1 cancels an order it calls `delete`, which returns memory to the OS. Both of those operations are slow and unpredictable. Phase 2 removes them completely.

The matching logic and public interface stay exactly the same. The only change is how memory for `OrderNode` objects is managed.

## The Problem With `new` and `delete`

When you call `new` in C++, the program asks the operating system for a block of memory. The OS allocator does several things: it searches a free list for a suitable block, it may need to lock that free list if other threads exist, it marks the block as used, and it returns a pointer. This process takes anywhere from 50 nanoseconds to several hundred microseconds depending on heap fragmentation and system load.

More importantly, it is unpredictable. The same `new` call might take 80 nanoseconds on one invocation and 80 microseconds on the next. For a matching engine where every microsecond matters, this randomness is unacceptable.

## The Solution: Pre-Allocate Everything at Startup

Instead of asking the OS for memory every time an order arrives, I ask the OS for a large block of memory exactly once at startup, before trading begins. I allocate one million `OrderNode` slots in a contiguous array. From that point on, every order allocation takes an index from this array. No OS involvement, no searching, no locking.

I track which slots are free using a free-list stack. At startup the stack contains indices `[0, 1, 2, ..., 999999]`. When I need a slot, I pop the top of the stack. When I free a slot, I push it back.

## How the Free List Moves

Imagine the pool starts with 5 slots. The free-list stack looks like this from top to bottom: `[0, 1, 2, 3, 4]`.

**Order A arrives.** I pop `0` from the stack. I write Order A's data into `pool[0]`. Stack is now `[1, 2, 3, 4]`.

**Order B arrives.** I pop `1`. I write Order B into `pool[1]`. Stack is now `[2, 3, 4]`.

**Order A is cancelled.** I push `0` back onto the stack. Stack is now `[0, 2, 3, 4]`. Slot 0 is back at the top and will be the next slot handed out. This is cache-friendly: that slot's memory is still warm in CPU cache from when Order A was written into it.

**Order C arrives.** I pop `0` again. I write Order C into `pool[0]`. Stack is `[2, 3, 4]`.

Every one of these operations is a single array access. No OS calls, no searching, no locking.

## The Flat Order Directory

Phase 1 used a `std::unordered_map<uint32_t, OrderNode*>` to look up orders by their ID when processing a cancel request. A hash map involves hashing the key, locating a bucket, and potentially walking a chain of collisions. In the worst case this is O(n). Even in the average case it involves pointer chasing that misses the CPU cache.

Phase 2 replaces this with a flat `std::vector<uint32_t>` of size one million, indexed directly by `order_id`. Looking up the slot index for order 42000 is now a single array access: `order_directory_[42000]`. The CPU computes the memory address as a simple multiply-and-add. No hashing, no pointer following, no cache misses.

The value stored is the pool slot index. If the order is live, the slot index is valid. If the order was cancelled or filled, the entry holds `INVALID` (the value `UINT32_MAX`, which is never a valid slot index because the pool only has one million slots).

## Code: The Memory Pool

I initialize the pool by chaining every slot's `next_idx` to the next slot. This turns the pre-allocated array itself into the free-list. No extra memory is needed.

```cpp
// src/memory_pool.cpp
MemoryPool::MemoryPool() : arena_(CAPACITY), next_free_(0) {
    for (uint32_t i = 0; i < CAPACITY - 1; ++i) {
        arena_[i].next_idx = i + 1;
    }
    arena_[CAPACITY - 1].next_idx = INVALID;
}
```

After this constructor runs, `arena_[0].next_idx == 1`, `arena_[1].next_idx == 2`, and so on. The free-list is embedded directly in the unused slots.

`alloc()` reads the current free head, advances the head to the next free slot, and returns the index.

```cpp
// src/memory_pool.cpp
uint32_t MemoryPool::alloc() {
    uint32_t slot = next_free_;
    next_free_    = arena_[slot].next_idx;  // Step the head forward
    return slot;                            // Give this slot to the caller
}
```

`free()` pushes the returned slot back to the front of the free-list.

```cpp
// src/memory_pool.cpp
void MemoryPool::free(uint32_t slot) {
    arena_[slot].next_idx = next_free_;  // Point this slot to the current head
    next_free_            = slot;         // Make this slot the new head
}
```

## Code: The Flat Order Directory

```cpp
// include/engine.hpp
static constexpr uint32_t MAX_ORDER_ID = 1'000'000;
std::vector<uint32_t> order_directory_;

// In the constructor, pre-fill everything with INVALID
order_directory_.resize(MAX_ORDER_ID, INVALID);

// Cancelling an order: single array lookup, no hashing
uint32_t slot = order_directory_[order_id];  // One instruction
if (slot == INVALID) return;                 // Already gone
OrderNode& node = pool_->get(slot);          // One more array access
```

## Why These Specific C++ Choices

**`std::vector<uint32_t>` for the order directory instead of `std::unordered_map`**

Phase 1 used `std::unordered_map<uint32_t, OrderNode*>` to find an order by ID during cancellations. A hash map call involves: computing the hash of the key, finding the bucket, and then walking a linked chain of entries if there are collisions. Even at average O(1), this involves pointer chasing that is guaranteed to cause a CPU cache miss because the hash bucket might live anywhere in memory.

I replaced this with a flat `std::vector<uint32_t>` of size one million, where the index is the `order_id`. Looking up slot for order 42000 is `order_directory_[42000]`, which the CPU computes as a base pointer plus a fixed offset. That is a single memory read and it is almost always a cache hit because the vector is contiguous memory.

```cpp
// Old approach (Phase 1): cache-unfriendly pointer chasing
OrderNode* node = order_map.at(order_id);  // Hash + bucket lookup + possible collision chain

// New approach (Phase 2): one array offset computation, cache-friendly
uint32_t slot = order_directory_[order_id]; // base_address + (order_id * sizeof(uint32_t))
```

**`order_directory_.resize(MAX_ORDER_ID, INVALID)`**

`resize` allocates the entire vector in one contiguous allocation and fills every element with `INVALID` (which is `UINT32_MAX`, the value `4294967295`). I chose `UINT32_MAX` as the sentinel because the pool only has one million slots, so `UINT32_MAX` can never be a valid slot index. This makes checking `if (slot == INVALID)` a completely safe and unambiguous guard.

**Why `MemoryPool::arena_` uses `std::vector` internally**

The pool's arena is `std::vector<OrderNode>` of capacity one million. I use `reserve()` in the constructor so the vector allocates its entire backing memory in a single `new` call. After that, `arena_[i]` is a direct array subscript — no heap allocation, no indirection. This is identical in performance to a raw `OrderNode arena[1000000]` C-style array but with the added safety that `std::vector` carries its own size and will not silently overflow.

## Terminal Output

The memory pool test verifies that after the engine starts and processes orders, zero additional heap allocations occur. Running `strace` on the process confirms that `brk` and `mmap` system calls (which the OS uses for heap allocation) do not appear during trading.
