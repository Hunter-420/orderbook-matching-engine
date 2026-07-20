# Phase 1: In-Memory Core Engine

## What This Phase Does

Phase 1 builds the matching engine's brain. There is no networking, no performance tuning, and no database. The only goal is making the matching logic provably correct. We read orders from a CSV file, process them one by one, and print the state of the book after each event.

Everything in the later phases rests on this foundation being right.

## The Core Concept: Price-Time Priority

A limit order book has two sides. The bid side holds buyers who want to buy at or below a certain price. The ask side holds sellers who want to sell at or above a certain price. When a buyer's price is high enough to meet a seller's price, those two orders match and a trade happens.

The order book enforces two rules simultaneously.

The first rule is price priority. A buyer willing to pay $102 gets filled before a buyer willing to pay $101 because the seller at $100 would rather deal with the more generous buyer. On the sell side, a seller asking $99 gets filled before a seller asking $100 because the buyer would rather pay less.

The second rule is time priority. When two orders have the exact same price, the one that arrived earlier gets filled first. This is fair: the trader who committed to a price first should be rewarded.

## A Concrete Walk-Through

Imagine three events happen in this exact order:

**Event 1:** Alice places a sell order for 100 shares at $101.

The engine checks the bid side. There are no buyers, so Alice's order rests on the ask side.

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

The engine always matches against the head of the price level queue. Alice is the head. The engine fills `min(120, 100) = 100` shares. Alice's order is fully consumed and removed. Charlie now has 20 shares remaining.

The engine looks again. Bob is now the head at $101. The engine fills `min(20, 50) = 20` shares. Bob still has 30 shares left. Charlie is now fully filled.

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

Notice that we never store timestamps on orders. Time priority is encoded structurally in the doubly linked list. The head of the list is always the oldest order. New orders always append to the tail. When the engine needs to match, it always reads the head. There is no sorting, no timestamp comparison, and no searching.

## Code: The Data Structures

Every live order is stored as an `OrderNode`. The price is kept as integer cents to avoid floating point rounding errors. The number `$100.01` cannot be represented exactly in binary floating point and becomes `100.00999...`, which would corrupt price comparisons. As an integer it is `10001`, which is always exact.

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

The engine holds two ordered maps. The bid map sorts prices from highest to lowest so `bids_.begin()` is always the best bid. The ask map sorts lowest to highest so `asks_.begin()` is always the best ask.

```cpp
// include/engine.hpp
// Bids: highest price first. bids_.begin() is always the best bid.
std::map<uint32_t, PriceLevel, std::greater<uint32_t>> bids_;

// Asks: lowest price first. asks_.begin() is always the best ask.
std::map<uint32_t, PriceLevel, std::less<uint32_t>> asks_;
```

## Code: The Matching Loop

When an incoming order arrives, the engine checks whether its price crosses the best opposing price. If it does, it calculates a fill quantity, decrements both sides, records the fill, and then checks whether the resting order was fully consumed. If it was, the engine unlinks it from the price level chain and frees its slot. Then the loop repeats with whatever quantity remains on the incoming order.

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

## Terminal Output

When you run the Phase 1 test you will see the book printed after each event. This lets you verify by eye that Alice is filling before Bob and that Bob's remaining quantity is correct.

![Phase 1 terminal output](screenshots/phase1_output.png)

*(Place a screenshot of your Phase 1 test output here)*
