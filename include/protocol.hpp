#pragma once

#include <cstdint>

#pragma pack(push, 1)

// All wire protocol structures are packed exactly.
// No compiler padding is allowed. 
// Endianness is assumed to be little-endian (native x86_64).

// Client -> Server message
struct OrderRequest {
    char     type;       // 'N' = New, 'C' = Cancel
    uint32_t order_id;   // 4 bytes
    uint32_t price;      // 4 bytes (integer cents)
    uint32_t quantity;   // 4 bytes
    char     side;       // 'B' = Buy, 'S' = Sell
};

// Server -> Client message
struct ExecutionReport {
    char     type;       // 'E' = Execution
    uint32_t order_id;   // 4 bytes
    uint32_t filled_qty; // 4 bytes
    uint32_t fill_price; // 4 bytes
    char     status;     // 'F' = Full fill, 'P' = Partial fill, 'C' = Cancelled, 'A' = Accepted
};

// Snapshot structs for visualizers
struct LevelData {
    uint32_t price;
    uint32_t quantity;
};

struct OrderbookSnapshot {
    char      type;      // 'O'
    uint32_t  num_bids;
    uint32_t  num_asks;
    LevelData bids[10];
    LevelData asks[10];
};

struct MemoryStateSnapshot {
    char     type;       // 'M'
    uint32_t next_free_idx;
    uint32_t total_active;
    uint32_t top_used_slots[10]; // First 10 slots to show physically used memory
};

#pragma pack(pop)

static_assert(sizeof(OrderRequest) == 14, "OrderRequest must be exactly 14 bytes");
static_assert(sizeof(ExecutionReport) == 14, "ExecutionReport must be exactly 14 bytes");
