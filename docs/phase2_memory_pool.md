# Phase 2: Memory Pool Optimization

The primary goal of Phase 2 is **performance**, specifically eliminating Operating System interventions on the critical matching path.

Calling `new` and `delete` invokes the OS heap allocator (`malloc` or similar). This takes hundreds of nanoseconds, requires thread synchronization deep inside the OS, and fragments memory, ruining CPU cache locality.

## Logic Explanation: The Free List Memory Pool
Instead of asking the OS for memory on every order, we ask the OS for memory **once** when the engine starts. We allocate a giant contiguous array (e.g., 1,000,000 slots). This acts as our custom memory arena.

To keep track of which slots are empty and which are used, we embed a **free list** into the unused slots.
When a slot is empty, its `next_idx` variable holds the index of the *next* empty slot. 
When we need memory, we don't search. We just pop the head off this free list chain in `O(1)` time. When we free an order, we push its slot index back onto the head of the chain.

### Example
Suppose we have a pool of 5 slots.
Initially, the free list looks like: `Head -> 0 -> 1 -> 2 -> 3 -> 4 -> INVALID`.
1. **Allocating an order:** We take the Head (Slot 0). The new Head becomes 1. 
   - `Head -> 1 -> 2 -> 3 -> 4 -> INVALID`.
2. **Allocating another order:** We take the Head (Slot 1). The new Head becomes 2.
   - `Head -> 2 -> 3 -> 4 -> INVALID`.
3. **Freeing Slot 0:** Order 0 is cancelled. We push Slot 0 back to the Head. 
   - `Head -> 0 -> 2 -> 3 -> 4 -> INVALID`.

This guarantees instant allocation without any kernel system calls.

## Code Snippets

### The Free List
```cpp
// src/memory_pool.cpp
MemoryPool::MemoryPool() : arena_(CAPACITY), next_free_(0) {
    // Chain every slot into the free list.
    // The arena is both the object store and the free list simultaneously.
    for (uint32_t i = 0; i < CAPACITY - 1; ++i) {
        arena_[i].next_idx = i + 1;
    }
    arena_[CAPACITY - 1].next_idx = INVALID;
}
```

### O(1) Allocation
To get a new slot, we simply pop the head of the free list.

```cpp
// src/memory_pool.cpp
uint32_t MemoryPool::alloc() {
    uint32_t slot = next_free_;
    next_free_    = arena_[slot].next_idx;  // Advance to next free slot
    return slot;
}
```

### O(1) Return
To free a slot, we push it to the front of the free list. Order matters!

```cpp
// src/memory_pool.cpp
void MemoryPool::free(uint32_t slot) {
    // Push to the front of the free list.
    arena_[slot].next_idx = next_free_;
    next_free_            = slot;
}
```

### Flat Order Directory
In Phase 1, we tracked live orders using `std::unordered_map`. But hash maps require hashing and bucket traversal (chasing pointers). In Phase 2, we replace it with a pre-allocated flat `std::vector` indexed directly by `order_id`. 

```cpp
// include/engine.hpp
// Lookups are now a single multiply-and-add instruction:
static constexpr uint32_t MAX_ORDER_ID = 1'000'000;
std::vector<uint32_t> order_directory_;

// src/engine.cpp (snippet from cancel_order)
uint32_t slot = order_directory_[order_id]; // Instant O(1) lookup
if (slot == INVALID) return;
OrderNode& node = pool_->get(slot);
```
