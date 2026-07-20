#include "engine.hpp"
#include "memory_pool.hpp"

#include <iostream>
#include <stdexcept>
#include <algorithm>

// ----------------------------------------------------------------------------
// Engine constructor / destructor
// ----------------------------------------------------------------------------

Engine::Engine() : pool_(new MemoryPool()) {
    // Pre-allocate the order directory to avoid any heap allocations
    // during trading. A direct vector index lookup replaces the
    // unordered_map hash/bucket traversal from Phase 1.
    order_directory_.resize(MAX_ORDER_ID, INVALID);
}

Engine::~Engine() {
    // The memory pool owns all storage for live orders.
    // Deleting the pool instantly frees all order nodes.
    delete pool_;
}

// ----------------------------------------------------------------------------
// Public interface
// ----------------------------------------------------------------------------

void Engine::new_order(uint32_t order_id, char side,
                       uint32_t price, uint32_t quantity) {
    
    if (order_id >= MAX_ORDER_ID) {
        std::cerr << "[engine] order_id " << order_id << " exceeds MAX_ORDER_ID\n";
        return;
    }

    if (order_directory_[order_id] != INVALID) {
        std::cerr << "[engine] duplicate order_id " << order_id << " rejected\n";
        return;
    }

    // O(1) allocation without OS involvement.
    uint32_t slot = pool_->alloc();
    OrderNode& node = pool_->get(slot);
    
    node.order_id = order_id;
    node.price    = price;
    node.quantity = quantity;
    node.side     = side;
    node.next_idx = INVALID;
    node.prev_idx = INVALID;

    // Try to match against the opposite side of the book.
    bool fully_filled = match(slot);

    if (!fully_filled) {
        // Remaining quantity rests in the book.
        add_to_book(slot);
    } else {
        // Fully filled on arrival: slot is no longer needed.
        pool_->free(slot);
    }
}

void Engine::cancel_order(uint32_t order_id) {
    if (order_id >= MAX_ORDER_ID) return;

    uint32_t slot = order_directory_[order_id];
    if (slot == INVALID) {
        // Order does not exist: already filled, cancelled, or never placed.
        return;
    }

    OrderNode& node = pool_->get(slot);

    // Chain healing must happen before slot recycling.
    if (node.side == 'B') {
        remove_from_bids(slot);
    } else {
        remove_from_asks(slot);
    }

    // Clear the directory entry, then return slot to the free list.
    order_directory_[order_id] = INVALID;
    pool_->free(slot);
}

std::vector<Fill> Engine::take_fills() {
    std::vector<Fill> out;
    out.swap(pending_fills_);
    return out;
}

// ----------------------------------------------------------------------------
// Private helpers
// ----------------------------------------------------------------------------

bool Engine::match(uint32_t incoming_slot) {
    OrderNode& incoming = pool_->get(incoming_slot);
    bool is_buy = (incoming.side == 'B');

    while (incoming.quantity > 0) {
        if (is_buy) {
            if (asks_.empty()) break;

            auto best_it = asks_.begin();
            uint32_t best_p = best_it->first;

            if (incoming.price < best_p) break;

            PriceLevel& level = best_it->second;
            uint32_t resting_slot = level.head_idx;
            OrderNode& resting = pool_->get(resting_slot);

            uint32_t fill = std::min(incoming.quantity, resting.quantity);
            incoming.quantity -= fill;
            resting.quantity  -= fill;
            level.total_volume -= fill;

            pending_fills_.push_back(
                Fill{incoming.order_id, resting.order_id, best_p, fill});

            if (resting.quantity == 0) {
                uint32_t next = resting.next_idx;
                if (next == INVALID) {
                    asks_.erase(best_it);
                } else {
                    level.head_idx = next;
                    pool_->get(next).prev_idx = INVALID;
                }
                order_directory_[resting.order_id] = INVALID;
                pool_->free(resting_slot);
            }
        } else {
            if (bids_.empty()) break;

            auto best_it = bids_.begin();
            uint32_t best_p = best_it->first;

            if (incoming.price > best_p) break;

            PriceLevel& level = best_it->second;
            uint32_t resting_slot = level.head_idx;
            OrderNode& resting = pool_->get(resting_slot);

            uint32_t fill = std::min(incoming.quantity, resting.quantity);
            incoming.quantity -= fill;
            resting.quantity  -= fill;
            level.total_volume -= fill;

            pending_fills_.push_back(
                Fill{resting.order_id, incoming.order_id, best_p, fill});

            if (resting.quantity == 0) {
                uint32_t next = resting.next_idx;
                if (next == INVALID) {
                    bids_.erase(best_it);
                } else {
                    level.head_idx = next;
                    pool_->get(next).prev_idx = INVALID;
                }
                order_directory_[resting.order_id] = INVALID;
                pool_->free(resting_slot);
            }
        }
    }

    return (incoming.quantity == 0);
}

void Engine::add_to_book(uint32_t slot) {
    OrderNode& node = pool_->get(slot);
    order_directory_[node.order_id] = slot;

    if (node.side == 'B') {
        auto it = bids_.find(node.price);
        if (it == bids_.end()) {
            bids_[node.price] = PriceLevel{slot, slot, node.quantity};
        } else {
            PriceLevel& level = it->second;
            uint32_t old_tail_slot = level.tail_idx;
            OrderNode& old_tail = pool_->get(old_tail_slot);
            
            old_tail.next_idx = slot;
            node.prev_idx     = old_tail_slot;
            level.tail_idx    = slot;
            level.total_volume += node.quantity;
        }
    } else {
        auto it = asks_.find(node.price);
        if (it == asks_.end()) {
            asks_[node.price] = PriceLevel{slot, slot, node.quantity};
        } else {
            PriceLevel& level = it->second;
            uint32_t old_tail_slot = level.tail_idx;
            OrderNode& old_tail = pool_->get(old_tail_slot);
            
            old_tail.next_idx = slot;
            node.prev_idx     = old_tail_slot;
            level.tail_idx    = slot;
            level.total_volume += node.quantity;
        }
    }
}

void Engine::remove_from_bids(uint32_t slot) {
    OrderNode& node = pool_->get(slot);
    auto it = bids_.find(node.price);
    if (it == bids_.end()) return;

    PriceLevel& level = it->second;
    level.total_volume -= node.quantity;

    if (node.prev_idx != INVALID) {
        pool_->get(node.prev_idx).next_idx = node.next_idx;
    } else {
        level.head_idx = node.next_idx;
    }

    if (node.next_idx != INVALID) {
        pool_->get(node.next_idx).prev_idx = node.prev_idx;
    } else {
        level.tail_idx = node.prev_idx;
    }

    if (level.head_idx == INVALID) {
        bids_.erase(it);
    }
}

void Engine::remove_from_asks(uint32_t slot) {
    OrderNode& node = pool_->get(slot);
    auto it = asks_.find(node.price);
    if (it == asks_.end()) return;

    PriceLevel& level = it->second;
    level.total_volume -= node.quantity;

    if (node.prev_idx != INVALID) {
        pool_->get(node.prev_idx).next_idx = node.next_idx;
    } else {
        level.head_idx = node.next_idx;
    }

    if (node.next_idx != INVALID) {
        pool_->get(node.next_idx).prev_idx = node.prev_idx;
    } else {
        level.tail_idx = node.prev_idx;
    }

    if (level.head_idx == INVALID) {
        asks_.erase(it);
    }
}

// ----------------------------------------------------------------------------
// Debug helper
// ----------------------------------------------------------------------------

void Engine::print_book() const {
    std::cout << "=== ORDER BOOK ===\n";

    std::cout << "  ASKS (lowest first):\n";
    for (auto& [price, level] : asks_) {
        std::cout << "    $" << price / 100.0 << ":";
        uint32_t idx = level.head_idx;
        while (idx != INVALID) {
            const OrderNode& node = pool_->get(idx);
            std::cout << " [ord=" << node.order_id
                      << " qty=" << node.quantity << "]";
            idx = node.next_idx;
        }
        std::cout << "  (total=" << level.total_volume << ")\n";
    }

    std::cout << "  BIDS (highest first):\n";
    for (auto& [price, level] : bids_) {
        std::cout << "    $" << price / 100.0 << ":";
        uint32_t idx = level.head_idx;
        while (idx != INVALID) {
            const OrderNode& node = pool_->get(idx);
            std::cout << " [ord=" << node.order_id
                      << " qty=" << node.quantity << "]";
            idx = node.next_idx;
        }
        std::cout << "  (total=" << level.total_volume << ")\n";
    }

    std::cout << "==================\n";
}

void Engine::get_orderbook_snapshot(OrderbookSnapshot& snapshot) const {
    snapshot.type = 'O';
    snapshot.num_bids = 0;
    snapshot.num_asks = 0;
    
    for (auto it = bids_.begin(); it != bids_.end() && snapshot.num_bids < 10; ++it) {
        snapshot.bids[snapshot.num_bids].price = it->first;
        snapshot.bids[snapshot.num_bids].quantity = it->second.total_volume;
        snapshot.num_bids++;
    }
    
    for (auto it = asks_.begin(); it != asks_.end() && snapshot.num_asks < 10; ++it) {
        snapshot.asks[snapshot.num_asks].price = it->first;
        snapshot.asks[snapshot.num_asks].quantity = it->second.total_volume;
        snapshot.num_asks++;
    }
}

void Engine::get_memory_snapshot(MemoryStateSnapshot& snapshot) const {
    snapshot.type = 'M';
    snapshot.next_free_idx = pool_->get_next_free();
    
    uint32_t active = 0;
    uint32_t slot_count = 0;
    for (uint32_t order_id = 0; order_id < MAX_ORDER_ID; ++order_id) {
        uint32_t slot = order_directory_[order_id];
        if (slot != INVALID) {
            if (slot_count < 10) {
                snapshot.top_used_slots[slot_count] = slot;
                slot_count++;
            }
            active++;
        }
    }
    snapshot.total_active = active;
}

void Engine::get_node_snapshot(NodeSnapshot& snapshot) const {
    snapshot.type = 'D';
    snapshot.num_nodes = 0;

    uint32_t active = 0;
    for (uint32_t order_id = 0; order_id < MAX_ORDER_ID; ++order_id) {
        uint32_t slot = order_directory_[order_id];
        if (slot == INVALID) continue;

        active++;
        if (snapshot.num_nodes < 10) {
            const OrderNode& node = pool_->get(slot);
            NodeData& nd = snapshot.nodes[snapshot.num_nodes];
            nd.slot_index = slot;
            nd.order_id   = node.order_id;
            nd.price      = node.price;
            nd.quantity   = node.quantity;
            nd.side       = node.side;
            nd._pad[0] = nd._pad[1] = nd._pad[2] = 0;
            nd.next_idx   = node.next_idx;
            nd.prev_idx   = node.prev_idx;
            snapshot.num_nodes++;
        }
    }
    snapshot.total_active = active;
}
