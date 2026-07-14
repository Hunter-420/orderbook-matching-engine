#pragma once

#include <cstdint>
#include <map>
#include <unordered_map>
#include <functional>
#include <string>
#include <vector>

// Sentinel value meaning "no node at this slot / end of chain"
static constexpr uint32_t INVALID = UINT32_MAX;

// One live order in the book.
//
// next_idx and prev_idx are pool slot indices rather than raw pointers.
// Using uint32_t (4 bytes) instead of a 64-bit pointer cuts link storage
// in half and lets more orders fit per cache line.
struct OrderNode {
    uint32_t order_id;
    uint32_t price;     // Integer representation: cents or ticks, never float
    uint32_t quantity;  // Remaining quantity; decreases on partial fills
    char     side;      // 'B' = buy, 'S' = sell
    uint32_t next_idx;  // Pool slot of the next order at this price. INVALID if tail.
    uint32_t prev_idx;  // Pool slot of the previous order at this price. INVALID if head.
};

// One active price level in the book.
//
// head_idx points to the oldest (highest priority) order.
// tail_idx points to the newest (lowest priority) order.
// Orders at the same price are chained head → tail in arrival order, so
// time priority is encoded structurally: no timestamps are needed.
struct PriceLevel {
    uint32_t head_idx;     // Slot of the oldest order at this price
    uint32_t tail_idx;     // Slot of the newest order at this price
    uint64_t total_volume; // Sum of all remaining quantities at this price
};

// A single execution report produced when a match occurs.
struct Fill {
    uint32_t buy_order_id;
    uint32_t sell_order_id;
    uint32_t price;
    uint32_t quantity;
};

// The matching engine.
//
// Phase 1 uses raw new/delete for OrderNode allocation and an
// std::unordered_map for the order directory. Both are replaced in Phase 2
// with the pre-allocated memory pool and a flat vector directory.
class Engine {
public:
    Engine();
    ~Engine();

    // Submit a new limit order. Matching happens immediately if prices cross.
    // Any unfilled remainder rests in the book.
    void new_order(uint32_t order_id, char side,
                   uint32_t price, uint32_t quantity);

    // Cancel an existing resting order by ID.
    // No-op if the order does not exist (already filled or never submitted).
    void cancel_order(uint32_t order_id);

    // Return all fills generated since the last call to this function.
    std::vector<Fill> take_fills();

    // Print the current state of both sides of the book to stdout.
    void print_book() const;

private:
    // Bids: highest price first. bids.begin() is always the best bid.
    std::map<uint32_t, PriceLevel, std::greater<uint32_t>> bids_;

    // Asks: lowest price first. asks.begin() is always the best ask.
    std::map<uint32_t, PriceLevel, std::less<uint32_t>> asks_;

    // Phase 1 order directory: maps order_id to a heap-allocated OrderNode.
    // Replaced with a flat vector<uint32_t> in Phase 2.
    std::unordered_map<uint32_t, OrderNode*> order_directory_;

    // Fills accumulated during the current round of matching.
    std::vector<Fill> pending_fills_;

    // Internal helpers

    // Run the matching loop for an incoming order against the opposite side.
    // Returns true if the incoming order was fully filled.
    bool match(OrderNode* incoming);

    // Place an unfilled (or partially filled) order onto the resting book.
    void add_to_book(OrderNode* node);

    // Remove a node from its price level queue and update the book maps.
    // This is chain healing: relink prev and next around the removed node,
    // update head/tail pointers, and erase the price level if now empty.
    // The caller is responsible for freeing the node afterward.
    void remove_from_level(OrderNode* node,
                           std::map<uint32_t, PriceLevel, std::greater<uint32_t>>& side_map);
    void remove_from_level(OrderNode* node,
                           std::map<uint32_t, PriceLevel, std::less<uint32_t>>& side_map);

    // Retrieve the OrderNode pointer for a given slot index.
    // In Phase 1 this is just the raw pointer stored in order_directory_.
    OrderNode* get_node(uint32_t slot);
};
