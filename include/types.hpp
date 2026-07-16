#pragma once

#include <cstdint>

// Sentinel value: "no node here", "end of chain", "slot not occupied".
// Used universally in next_idx, prev_idx, head_idx, tail_idx, and the
// order directory vector. Chosen as UINT32_MAX so it is never a valid slot
// index into a pool of one million slots.
static constexpr uint32_t INVALID = UINT32_MAX;

// One live order in the book.
//
// next_idx and prev_idx are pool slot indices, not raw pointers.
// 4 bytes per link instead of 8 means 3 nodes fit in a 64-byte cache line
// where pointer links would fit only 2. Over millions of traversals per second
// during matching, fitting one more order per cache line is measurable.
//
// Price is stored as integer cents ($101.00 = 10100) to avoid floating-point
// representation errors. 100.01 as a float is internally 100.009999...,
// which corrupts equality comparisons used in price-crossing checks.
struct OrderNode {
    uint32_t order_id;   // Application-level identifier from the client
    uint32_t price;      // Integer cents: $101.00 stored as 10100
    uint32_t quantity;   // Remaining shares; decreases on each partial fill
    char     side;       // 'B' = buy, 'S' = sell
    char     _pad[3];    // Explicit padding for documentation clarity
    uint32_t next_idx;   // Pool slot of the next order at this price. INVALID = tail.
    uint32_t prev_idx;   // Pool slot of the previous order at this price. INVALID = head.
};

// One active price level in the order book.
//
// The doubly linked list of orders at this price runs head_idx to tail_idx.
// Head is the oldest (highest priority) order; it is always matched first.
// Tail is the newest order; it is appended when a new order rests at this price.
// Time priority is encoded structurally in this ordering, not in timestamps.
struct PriceLevel {
    uint32_t head_idx;     // Pool slot of the oldest resting order at this price
    uint32_t tail_idx;     // Pool slot of the newest resting order at this price
    uint64_t total_volume; // Sum of all remaining quantities; maintained on every change
};

// A single match event between one buyer and one seller.
struct Fill {
    uint32_t buy_order_id;
    uint32_t sell_order_id;
    uint32_t price;
    uint32_t quantity;
};
