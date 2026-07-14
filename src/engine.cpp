#include "engine.hpp"

#include <iostream>
#include <stdexcept>
#include <algorithm>

// ----------------------------------------------------------------------------
// Engine constructor / destructor
// ----------------------------------------------------------------------------

Engine::Engine() = default;

Engine::~Engine() {
    // Free all heap-allocated nodes still resting in the book.
    // In Phase 2 this cleanup disappears because the memory pool owns
    // all storage and is destroyed with the Engine object.
    for (auto& [id, node] : order_directory_) {
        delete node;
    }
}

// ----------------------------------------------------------------------------
// Public interface
// ----------------------------------------------------------------------------

void Engine::new_order(uint32_t order_id, char side,
                       uint32_t price, uint32_t quantity) {
    // Reject duplicate order IDs.
    if (order_directory_.count(order_id)) {
        std::cerr << "[engine] duplicate order_id " << order_id << " rejected\n";
        return;
    }

    // Allocate a node for the incoming order. Phase 1 uses the heap.
    // Phase 2 replaces this with pool.alloc() + pool.get(slot) field writes.
    OrderNode* node = new OrderNode{};
    node->order_id  = order_id;
    node->price     = price;
    node->quantity  = quantity;
    node->side      = side;
    node->next_idx  = INVALID;
    node->prev_idx  = INVALID;

    // Try to match against the opposite side of the book.
    bool fully_filled = match(node);

    if (!fully_filled) {
        // Remaining quantity rests in the book.
        add_to_book(node);
    } else {
        // Fully filled on arrival: the node is not needed in the book.
        delete node;
    }
}

void Engine::cancel_order(uint32_t order_id) {
    auto it = order_directory_.find(order_id);
    if (it == order_directory_.end()) {
        // Order does not exist: already filled, already cancelled, or never placed.
        return;
    }

    OrderNode* node = it->second;

    // Chain healing must happen before slot recycling.
    // Remove the node from its side's price level map.
    if (node->side == 'B') {
        remove_from_level(node, bids_);
    } else {
        remove_from_level(node, asks_);
    }

    // Clear the directory entry, then free the heap node.
    order_directory_.erase(it);
    delete node;
}

std::vector<Fill> Engine::take_fills() {
    std::vector<Fill> out;
    out.swap(pending_fills_);
    return out;
}

// ----------------------------------------------------------------------------
// Private helpers
// ----------------------------------------------------------------------------

// Run the matching loop.
// Returns true if the incoming order is fully consumed (quantity reaches 0).
bool Engine::match(OrderNode* incoming) {
    bool is_buy     = (incoming->side == 'B');
    // Choose the opposite side of the book.
    auto& opp_bids  = bids_;
    auto& opp_asks  = asks_;

    while (incoming->quantity > 0) {
        if (is_buy) {
            // Buy side: match against the lowest ask.
            if (opp_asks.empty()) break;

            auto best_it    = opp_asks.begin();
            uint32_t best_p = best_it->first;

            // Prices cross when the buy price is at least the ask price.
            if (incoming->price < best_p) break;

            PriceLevel& level   = best_it->second;
            OrderNode*  resting = get_node(level.head_idx);

            uint32_t fill = std::min(incoming->quantity, resting->quantity);
            incoming->quantity -= fill;
            resting->quantity  -= fill;
            level.total_volume -= fill;

            pending_fills_.push_back(
                Fill{incoming->order_id, resting->order_id, best_p, fill});

            if (resting->quantity == 0) {
                // Resting order fully consumed: advance the head pointer.
                uint32_t next = resting->next_idx;
                if (next == INVALID) {
                    opp_asks.erase(best_it);  // Price level is now empty.
                } else {
                    level.head_idx = next;
                    get_node(next)->prev_idx = INVALID;
                }
                order_directory_.erase(resting->order_id);
                delete resting;
            }

        } else {
            // Sell side: match against the highest bid.
            if (opp_bids.empty()) break;

            auto best_it    = opp_bids.begin();
            uint32_t best_p = best_it->first;

            // Prices cross when the sell price is at most the bid price.
            if (incoming->price > best_p) break;

            PriceLevel& level   = best_it->second;
            OrderNode*  resting = get_node(level.head_idx);

            uint32_t fill = std::min(incoming->quantity, resting->quantity);
            incoming->quantity -= fill;
            resting->quantity  -= fill;
            level.total_volume -= fill;

            pending_fills_.push_back(
                Fill{resting->order_id, incoming->order_id, best_p, fill});

            if (resting->quantity == 0) {
                uint32_t next = resting->next_idx;
                if (next == INVALID) {
                    opp_bids.erase(best_it);
                } else {
                    level.head_idx = next;
                    get_node(next)->prev_idx = INVALID;
                }
                order_directory_.erase(resting->order_id);
                delete resting;
            }
        }
    }

    return (incoming->quantity == 0);
}

// Append node to the tail of its price level queue.
// If the price level does not exist yet, create it with head = tail = this node.
void Engine::add_to_book(OrderNode* node) {
    order_directory_[node->order_id] = node;

    if (node->side == 'B') {
        auto it = bids_.find(node->price);
        if (it == bids_.end()) {
            // New price level.
            bids_[node->price] = PriceLevel{node->order_id, node->order_id,
                                             node->quantity};
        } else {
            // Existing price level: append to tail.
            PriceLevel& level      = it->second;
            OrderNode*  old_tail   = get_node(level.tail_idx);
            old_tail->next_idx     = node->order_id;  // Phase 1: node ID == slot index
            node->prev_idx         = level.tail_idx;
            level.tail_idx         = node->order_id;
            level.total_volume    += node->quantity;
        }
    } else {
        auto it = asks_.find(node->price);
        if (it == asks_.end()) {
            asks_[node->price] = PriceLevel{node->order_id, node->order_id,
                                             node->quantity};
        } else {
            PriceLevel& level      = it->second;
            OrderNode*  old_tail   = get_node(level.tail_idx);
            old_tail->next_idx     = node->order_id;
            node->prev_idx         = level.tail_idx;
            level.tail_idx         = node->order_id;
            level.total_volume    += node->quantity;
        }
    }
}

// Remove a node from the bid side's price level chain.
// Relinks prev and next neighbours around the removed node.
// Clears the price level from the map if it becomes empty.
void Engine::remove_from_level(
    OrderNode* node,
    std::map<uint32_t, PriceLevel, std::greater<uint32_t>>& side_map) {

    auto it = side_map.find(node->price);
    if (it == side_map.end()) return;

    PriceLevel& level = it->second;
    level.total_volume -= node->quantity;

    // Relink the prev neighbour.
    if (node->prev_idx != INVALID) {
        get_node(node->prev_idx)->next_idx = node->next_idx;
    } else {
        level.head_idx = node->next_idx;  // Removed node was the head.
    }

    // Relink the next neighbour.
    if (node->next_idx != INVALID) {
        get_node(node->next_idx)->prev_idx = node->prev_idx;
    } else {
        level.tail_idx = node->prev_idx;  // Removed node was the tail.
    }

    // If the level is now empty, remove it from the map.
    if (level.head_idx == INVALID) {
        side_map.erase(it);
    }
}

// Remove a node from the ask side's price level chain.
void Engine::remove_from_level(
    OrderNode* node,
    std::map<uint32_t, PriceLevel, std::less<uint32_t>>& side_map) {

    auto it = side_map.find(node->price);
    if (it == side_map.end()) return;

    PriceLevel& level = it->second;
    level.total_volume -= node->quantity;

    if (node->prev_idx != INVALID) {
        get_node(node->prev_idx)->next_idx = node->next_idx;
    } else {
        level.head_idx = node->next_idx;
    }

    if (node->next_idx != INVALID) {
        get_node(node->next_idx)->prev_idx = node->prev_idx;
    } else {
        level.tail_idx = node->prev_idx;
    }

    if (level.head_idx == INVALID) {
        side_map.erase(it);
    }
}

// In Phase 1, next_idx and prev_idx store order IDs, which also serve as
// lookup keys in the order_directory_ map. get_node() resolves an ID to
// its heap pointer. Phase 2 replaces this with a direct pool slot index.
OrderNode* Engine::get_node(uint32_t id) {
    auto it = order_directory_.find(id);
    if (it == order_directory_.end()) {
        throw std::runtime_error("[engine] get_node: id not in directory");
    }
    return it->second;
}

// ----------------------------------------------------------------------------
// Debug helper
// ----------------------------------------------------------------------------

void Engine::print_book() const {
    std::cout << "=== ORDER BOOK ===\n";

    std::cout << "  ASKS (lowest first):\n";
    for (auto& [price, level] : asks_) {
        // Traverse the chain from head to tail.
        std::cout << "    $" << price / 100.0 << ":";
        uint32_t idx = level.head_idx;
        while (idx != INVALID) {
            auto it = order_directory_.find(idx);
            if (it == order_directory_.end()) break;
            std::cout << " [ord=" << it->second->order_id
                      << " qty=" << it->second->quantity << "]";
            idx = it->second->next_idx;
        }
        std::cout << "  (total=" << level.total_volume << ")\n";
    }

    std::cout << "  BIDS (highest first):\n";
    for (auto& [price, level] : bids_) {
        std::cout << "    $" << price / 100.0 << ":";
        uint32_t idx = level.head_idx;
        while (idx != INVALID) {
            auto it = order_directory_.find(idx);
            if (it == order_directory_.end()) break;
            std::cout << " [ord=" << it->second->order_id
                      << " qty=" << it->second->quantity << "]";
            idx = it->second->next_idx;
        }
        std::cout << "  (total=" << level.total_volume << ")\n";
    }

    std::cout << "==================\n";
}
