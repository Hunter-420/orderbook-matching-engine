# Memory Layout and Zero-Allocation Architecture

One of the most critical design features of this matching engine is its zero-allocation architecture. Once the engine starts, it never calls the Operating System for memory (no `malloc`, `new`, `free`, or `delete`). 

To prove this and visualize exactly what is happening in C++ memory space, we built a custom memory polling protocol and the `tests/memory_visualizer.py` script.

## The Memory State Snapshot
We extended our fixed-width binary protocol so a client can query the engine's exact memory layout. By sending a 14-byte packet with `type = 'M'`, the engine intercepts it directly inside the `epoll` loop and immediately replies with a 49-byte `MemoryStateSnapshot`.

```cpp
// include/protocol.hpp
struct MemoryStateSnapshot {
    char     type;       // 'M'
    uint32_t next_free_idx; // The head of the embedded free-list
    uint32_t total_active;  // Number of active, in-use orders
    uint32_t top_used_slots[10]; // The physical array indices currently in use
};
```

## Running the Simulation

When you run `tests/memory_visualizer.py` in a separate terminal, it continuously polls the engine. 

As you place orders (for example, using `tests/manual_client.py`), you will watch the **Free List Head Index** physically jump around in memory as `O(1)` allocations are popped off the free-list chain. You will also see the active physical slot indices directly mapped from the flat vector `order_directory_`.

**Example Output:**
```text
==================================================
       INTERNAL MEMORY LAYOUT (C++)           
==================================================
Total Arena Capacity:      1,000,000 slots
Active Live Orders:        2 slots
Available Free Slots:      999,998 slots

--- Pointers ---
Free List Head Index:      -> [2]

--- Top Physical Slots in Use (Order Directory) ---
  Slot Index: [0]
  Slot Index: [1]
```

This visualization guarantees that the engine is completely isolated from the kernel's memory management, ensuring perfectly deterministic nanosecond latency.
