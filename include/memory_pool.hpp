#pragma once

#include "types.hpp"

#include <cstdint>
#include <stdexcept>
#include <vector>

// MemoryPool: pre-allocated flat arena of OrderNode slots.
//
// The OS is asked for all memory exactly once, in the constructor, via the
// std::vector constructor. No heap allocation happens during trading.
//
// Free slots are tracked with an embedded free list. Each free slot reuses
// its own next_idx field to hold the index of the next free slot, forming
// a singly linked chain. The head of that chain is next_free_. Allocating
// a slot pops from the head in O(1). Returning a slot pushes to the head
// in O(1). No searching, no OS call, no lock.
//
// Slot indices are uint32_t, matching the next_idx and prev_idx fields in
// OrderNode. This keeps every link at 4 bytes instead of 8, fitting more
// orders into a cache line during matching traversal.
class MemoryPool {
public:
    // One million slots covers all realistic intraday order volume.
    // At 24 bytes per OrderNode the arena occupies 24 MB, which fits
    // comfortably in L3 cache on modern server hardware.
    static constexpr uint32_t CAPACITY = 1'000'000;

    // Build the arena and chain every slot into the initial free list.
    // slot[0].next_idx = 1, slot[1].next_idx = 2, ..., slot[N-1].next_idx = INVALID.
    MemoryPool();

    // Claim the next free slot. Returns its index into the arena.
    // O(1): reads next_free_, advances it along the chain, returns old head.
    // Throws if the pool is exhausted (all slots are live orders).
    uint32_t alloc();

    // Return slot back to the free list.
    // O(1): links the slot to the front of the free list, updates next_free_.
    // The CORRECT order is:  arena[slot].next_idx = next_free_  THEN  next_free_ = slot.
    // Reversing these two steps permanently loses the rest of the chain.
    void free(uint32_t slot);

    // Direct slot access by index. Used by the engine to read and write
    // OrderNode fields after alloc() reserves the slot.
    OrderNode& get(uint32_t slot);
    const OrderNode& get(uint32_t slot) const;

private:
    // Flat contiguous storage. Allocated once, never reallocated.
    std::vector<OrderNode> arena_;

    // Index of the first available slot. INVALID means no slots remain.
    uint32_t next_free_;
};
