# Phase 2: Memory Pool Optimization

The primary goal of Phase 2 is **performance**, specifically eliminating Operating System interventions on the critical matching path.

Calling `new` and `delete` invokes the OS heap allocator (`malloc` or similar). This takes hundreds of nanoseconds, requires thread synchronization deep inside the OS, and fragments memory, ruining CPU cache locality.

## Design

Instead of asking the OS for memory on every order, we ask the OS for memory **once** when the engine starts. We allocate a giant `std::vector` of 1,000,000 `OrderNode` slots.

This is our `MemoryPool`.

## Code Snippets

### The Free List
To allocate slots in `O(1)` time without searching, we use an embedded free list. Every unused slot uses its `next_idx` variable to point to the *next* unused slot. 

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
