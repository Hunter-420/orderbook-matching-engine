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

#pragma pack(pop)

static_assert(sizeof(OrderRequest) == 14, "OrderRequest must be exactly 14 bytes");
static_assert(sizeof(ExecutionReport) == 14, "ExecutionReport must be exactly 14 bytes");
