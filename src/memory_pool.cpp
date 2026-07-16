#include "memory_pool.hpp"

#include <stdexcept>

// ----------------------------------------------------------------------------
// Construction
// ----------------------------------------------------------------------------

MemoryPool::MemoryPool()
    : arena_(CAPACITY), next_free_(0) {

    // Chain every slot into the free list by writing into the next_idx field
    // that the OrderNode struct already provides. No extra metadata storage
    // is needed. The arena is the free list and the object store at the same
    // time; when a slot is live its next_idx is an order book chain link, and
    // when a slot is free its next_idx is a free-list chain link.
    for (uint32_t i = 0; i < CAPACITY - 1; ++i) {
        arena_[i].next_idx = i + 1;
    }
    arena_[CAPACITY - 1].next_idx = INVALID;
}

// ----------------------------------------------------------------------------
// Slot allocation
// ----------------------------------------------------------------------------

uint32_t MemoryPool::alloc() {
    if (next_free_ == INVALID) {
        throw std::runtime_error("MemoryPool::alloc: pool exhausted");
    }

    // Pop the head of the free list.
    uint32_t slot = next_free_;
    next_free_     = arena_[slot].next_idx;  // Advance to next free slot.
    return slot;
}

// ----------------------------------------------------------------------------
// Slot return
// ----------------------------------------------------------------------------

void MemoryPool::free(uint32_t slot) {
    // Push to the front of the free list.
    // ORDER MATTERS: write next_idx into the returning slot first, THEN
    // update next_free_. Doing it in the opposite order loses every currently
    // free slot because next_free_ would point at the returning slot before
    // the returning slot's next_idx is set, breaking the chain permanently.
    arena_[slot].next_idx = next_free_;
    next_free_            = slot;
}

// ----------------------------------------------------------------------------
// Slot access
// ----------------------------------------------------------------------------

OrderNode& MemoryPool::get(uint32_t slot) {
    return arena_[slot];
}

const OrderNode& MemoryPool::get(uint32_t slot) const {
    return arena_[slot];
}
