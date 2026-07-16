#pragma once

#include "types.hpp"

#include <cstdint>
#include <map>
#include <functional>
#include <vector>

// Forward-declare MemoryPool so engine.hpp compiles without pulling in the
// full memory_pool.hpp definition. This avoids a circular include:
//   engine.hpp needs MemoryPool
//   memory_pool.hpp needs OrderNode (from types.hpp, not engine.hpp)
class MemoryPool;

// The matching engine.
//
// Phase 2 changes from Phase 1:
//   OrderNode allocation: pool_->alloc() and pool_->free() replace new/delete.
//   Order directory: flat vector<uint32_t> indexed by order_id replaces
//     unordered_map. Direct index calculation replaces hash + bucket lookup.
//   get_node (internal): pool_->get(slot) replaces directory pointer lookup.
//
// The public interface is unchanged from Phase 1.
class Engine {
public:
    Engine();
    ~Engine();

    // Prevent copying. The engine exclusively owns a MemoryPool and live state.
    Engine(const Engine&)            = delete;
    Engine& operator=(const Engine&) = delete;

    // Submit a new limit order. Matching executes immediately if prices cross.
    // Any unfilled remainder rests at the tail of its price level queue.
    void new_order(uint32_t order_id, char side,
                   uint32_t price, uint32_t quantity);

    // Cancel a resting order by application-level order_id.
    // No-op if the order is not found (fully filled, previously cancelled,
    // or never submitted).
    void cancel_order(uint32_t order_id);

    // Return and clear all fills generated since the last call.
    std::vector<Fill> take_fills();

    // Print both sides of the book to stdout for debugging and validation.
    void print_book() const;

private:
    // Pre-allocated arena. All OrderNode storage lives here for the engine's
    // lifetime. Heap allocation on the hot path does not occur in Phase 2+.
    MemoryPool* pool_;

    // Bids: highest price first. bids_.begin() is always the best bid.
    std::map<uint32_t, PriceLevel, std::greater<uint32_t>> bids_;

    // Asks: lowest price first. asks_.begin() is always the best ask.
    std::map<uint32_t, PriceLevel, std::less<uint32_t>> asks_;

    // Order directory: maps order_id directly to the pool slot index where
    // that order's OrderNode lives. A flat vector<uint32_t> allows O(1)
    // lookup as a single multiply-and-add address calculation. The
    // unordered_map in Phase 1 required hashing + possible bucket traversal.
    // Entry is INVALID when no live order exists for that id.
    static constexpr uint32_t MAX_ORDER_ID = 1'000'000;
    std::vector<uint32_t> order_directory_;

    // Fills accumulated during the current matching cycle.
    std::vector<Fill> pending_fills_;

    // Run the matching loop for the order at incoming_slot.
    // Returns true if the order is fully filled (quantity reaches zero).
    bool match(uint32_t incoming_slot);

    // Place an order with remaining quantity onto the resting book.
    // Appends to the tail of the appropriate price level queue.
    void add_to_book(uint32_t slot);

    // Remove a slot from the bid-side price level chain.
    // Relinks prev/next neighbours around the removed slot.
    // Updates head_idx/tail_idx if the removed slot was the head or tail.
    // Erases the PriceLevel entry from the map if the level becomes empty.
    // Chain healing MUST complete before pool_->free(slot) is called.
    void remove_from_bids(uint32_t slot);

    // Remove a slot from the ask-side price level chain.
    void remove_from_asks(uint32_t slot);
};
