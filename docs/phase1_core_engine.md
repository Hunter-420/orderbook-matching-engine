# Phase 1: In-Memory Core Engine

The primary goal of Phase 1 is **correctness**. We build the core logic of the limit order book, proving that price-time priority matching works perfectly before adding performance optimizations.

## Design

The limit order book holds resting orders that have not yet been matched. We need to match incoming orders against the opposite side of the book at the best possible price. 
To do this, we maintain two maps:
1. **Bids (Buy side):** Sorted highest price first.
2. **Asks (Sell side):** Sorted lowest price first.

## Code Snippets

### The Order Node
Instead of massive objects, our `OrderNode` is tightly packed. We use integers for prices (cents) to avoid floating-point errors (e.g. `$100.01` becoming `100.00999...`).

```cpp
// include/types.hpp
struct OrderNode {
    uint32_t order_id;   // Application-level identifier
    uint32_t price;      // Integer cents: $101.00 stored as 10100
    uint32_t quantity;   // Remaining shares; decreases on partial fill
    char     side;       // 'B' = buy, 'S' = sell
    char     _pad[3];    // Padding
    uint32_t next_idx;   // Next order at this price
    uint32_t prev_idx;   // Previous order at this price
};
```

### The Price Level
Orders at the exact same price are queued in a doubly linked list. The `PriceLevel` struct points to the head (oldest order) and tail (newest order). Time priority is inherently preserved structurally.

```cpp
// include/types.hpp
struct PriceLevel {
    uint32_t head_idx;     // Oldest order (matched first)
    uint32_t tail_idx;     // Newest order (matched last)
    uint64_t total_volume; // Sum of all remaining quantities at this price
};
```

### Engine Data Structures
The engine holds the maps of `PriceLevel` objects. Notice the custom sorting functions: `std::greater` for bids, `std::less` for asks. This ensures `begin()` always points to the best price.

```cpp
// include/engine.hpp
class Engine {
    // ...
private:
    // Bids: highest price first. bids_.begin() is always the best bid.
    std::map<uint32_t, PriceLevel, std::greater<uint32_t>> bids_;

    // Asks: lowest price first. asks_.begin() is always the best ask.
    std::map<uint32_t, PriceLevel, std::less<uint32_t>> asks_;
    
    // Order directory for O(1) cancellations
    std::vector<uint32_t> order_directory_;
};
```

### The Matching Loop
When an order arrives, we check if it crosses the spread. If it does, we generate a fill and deduct the quantity.

```cpp
// src/engine.cpp (snippet from match)
uint32_t fill = std::min(incoming.quantity, resting.quantity);
incoming.quantity -= fill;
resting.quantity  -= fill;
level.total_volume -= fill;

pending_fills_.push_back(
    Fill{incoming.order_id, resting.order_id, best_p, fill});

if (resting.quantity == 0) {
    // Order is fully filled, remove from price level chain
    uint32_t next = resting.next_idx;
    if (next == INVALID) {
        asks_.erase(best_it); // Price level empty, remove it
    } else {
        level.head_idx = next;
        pool_->get(next).prev_idx = INVALID;
    }
}
```
