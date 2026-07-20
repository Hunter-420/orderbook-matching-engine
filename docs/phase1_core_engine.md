# Phase 1: In-Memory Core Engine

## What This Phase Does

Phase 1 builds the matching engine's brain. There is no networking, no performance tuning, and no database. The only goal is making the matching logic provably correct. I read orders from a CSV file, process them one by one, and print the state of the book after each event.

Everything in the later phases rests on this foundation being right.

## The Core Concept: Price-Time Priority

A limit order book has two sides. The bid side holds buyers who want to buy at or below a certain price. The ask side holds sellers who want to sell at or above a certain price. When a buyer's price is high enough to meet a seller's price, those two orders match and a trade happens.

I enforce two rules simultaneously.

The first rule is price priority. A buyer willing to pay $102 gets filled before a buyer willing to pay $101 because the seller at $100 would rather deal with the more generous buyer. On the sell side, a seller asking $99 gets filled before a seller asking $100 because the buyer would rather pay less.

The second rule is time priority. When two orders have the exact same price, the one that arrived earlier gets filled first. This is fair: the trader who committed to a price first should be rewarded.

## A Concrete Example

Three events happen in this exact order.

**Event 1:** Alice places a sell order for 100 shares at $101.

I check the bid side. There are no buyers, so Alice's order rests on the ask side.

```
ASK SIDE: $101 -> [Alice: 100 shares]
BID SIDE: (empty)
```

**Event 2:** Bob places a sell order for 50 shares at $101.

Again no buyers. Bob's order joins Alice's at the same price level. Because Alice arrived first, she is at the front of the queue.

```
ASK SIDE: $101 -> [Alice: 100] -> [Bob: 50]
BID SIDE: (empty)
```

**Event 3:** Charlie places a buy order for 120 shares at $101.

Now prices cross. Charlie's price ($101) is equal to the best ask ($101) so matching begins.

I always match against the head of the price level queue. Alice is the head. I fill `min(120, 100) = 100` shares. Alice's order is fully consumed and removed. Charlie now has 20 shares remaining.

I look again. Bob is now the head at $101. I fill `min(20, 50) = 20` shares. Bob still has 30 shares left. Charlie is now fully filled.

Final state of the book:

```
ASK SIDE: $101 -> [Bob: 30]   (Alice is gone, Bob has 30 remaining)
BID SIDE: (empty)

Fills generated:
  Alice sold 100 shares to Charlie at $101
  Bob sold 20 shares to Charlie at $101
```

This is price-time priority working exactly as designed.

## How Time Priority Is Stored

I never store timestamps on orders. Time priority is encoded structurally in the doubly linked list. The head of the list is always the oldest order. New orders always append to the tail. When I need to match, I always read the head. There is no sorting, no timestamp comparison, and no searching.

## Code: The Data Structures

I store every live order as an `OrderNode`. The price is kept as integer cents to avoid floating point rounding errors. The number `$100.01` cannot be represented exactly in binary floating point and becomes `100.00999...`, which would corrupt price comparisons. As an integer it is `10001`, which is always exact.

```cpp
// include/types.hpp
struct OrderNode {
    uint32_t order_id;   // Which order this is
    uint32_t price;      // In integer cents: $101.00 is stored as 10100
    uint32_t quantity;   // Remaining shares (decreases as partial fills happen)
    char     side;       // 'B' for buy, 'S' for sell
    char     _pad[3];    // Padding to align the next field
    uint32_t next_idx;   // Pool slot of the next order at this price (INVALID = none)
    uint32_t prev_idx;   // Pool slot of the previous order at this price (INVALID = none)
};
```

Each price level that has at least one resting order gets a `PriceLevel` entry. It tracks the head and tail of its linked list and keeps a running total of all shares at that price.

```cpp
// include/types.hpp
struct PriceLevel {
    uint32_t head_idx;     // Pool slot of the oldest order (matched first)
    uint32_t tail_idx;     // Pool slot of the newest order (matched last)
    uint64_t total_volume; // Sum of all resting quantities at this price
};
```

I hold two ordered maps. The bid map sorts prices from highest to lowest so `bids_.begin()` is always the best bid. The ask map sorts lowest to highest so `asks_.begin()` is always the best ask.

```cpp
// include/engine.hpp
// Bids: highest price first. bids_.begin() is always the best bid.
std::map<uint32_t, PriceLevel, std::greater<uint32_t>> bids_;

// Asks: lowest price first. asks_.begin() is always the best ask.
std::map<uint32_t, PriceLevel, std::less<uint32_t>> asks_;
```

## Code: The Matching Loop

When an incoming order arrives, I check whether its price crosses the best opposing price. If it does, I calculate a fill quantity, decrement both sides, record the fill, and then check whether the resting order was fully consumed. If it was, I unlink it from the price level chain and free its slot. Then the loop repeats with whatever quantity remains on the incoming order.

```cpp
// src/engine.cpp — inside the match() function
uint32_t fill = std::min(incoming.quantity, resting.quantity);
incoming.quantity -= fill;
resting.quantity  -= fill;
level.total_volume -= fill;

pending_fills_.push_back(
    Fill{incoming.order_id, resting.order_id, best_p, fill});

if (resting.quantity == 0) {
    uint32_t next = resting.next_idx;
    if (next == INVALID) {
        // This was the only order at this price. Remove the entire level.
        asks_.erase(best_it);
    } else {
        // There are more orders behind this one. Advance the head.
        level.head_idx = next;
        pool_->get(next).prev_idx = INVALID;
    }
    order_directory_[resting.order_id] = INVALID;
    pool_->free(resting_slot);
}
```

## Why These Specific C++ Choices

**`std::map` with `std::greater<uint32_t>` for the bid book**

`std::map` is a Red-Black tree. It keeps every key sorted at all times. I use `std::greater` as the comparator so the map sorts prices from highest to lowest. This means `bids_.begin()` always points to the best (highest) bid in O(1) — no searching required.

The alternative, `std::unordered_map`, is a hash table. Hash tables have O(1) average lookup by a specific key, but they have no concept of ordering. To find the maximum price in an unordered_map I would need to scan every element, which is O(N). In a hot matching loop that runs thousands of times per second, O(N) would be fatal.

`std::map` insertion and deletion are O(log N) rather than O(1), but since price levels are far fewer than individual orders, this is the correct trade-off.

```cpp
// bids_.begin() is always the highest price. No searching, no scanning.
auto best_bid_it = bids_.begin();
uint32_t best_price = best_bid_it->first;
PriceLevel& level = best_bid_it->second;
```

**`std::min` in the matching loop**

`std::min(incoming.quantity, resting.quantity)` calculates how many shares the current match can consume. I cannot fill more than either side has. Using `std::min` makes the partial fill logic a single expression with no branching.

```cpp
// If incoming wants 120 shares and resting has 100, fill = 100.
// Both orders update: resting goes to 0 (removed), incoming drops to 20.
uint32_t fill = std::min(incoming.quantity, resting.quantity);
```

**`pending_fills_.push_back()`**

I collect all fills into a `std::vector<Fill>` during the matching loop rather than sending them to clients immediately. This separates the matching logic (which must be fast and deterministic) from the I/O logic (which involves system calls). After the matching loop completes, the event loop in `main.cpp` calls `engine.take_fills()` to drain the vector and sends the execution reports over TCP.

**`asks_.erase(best_it)` vs advancing `head_idx`**

When the last order at a price level is consumed I erase the entire price level entry from the map. If there are more orders at that price I only advance `head_idx` to the next node. Erasing avoids keeping empty price levels in the tree, which would slow down `begin()` traversal over time.

## Terminal Output

Running the Phase 1 test prints the book after each event, which lets you verify by eye that Alice fills before Bob and that Bob's remaining quantity is correct.
