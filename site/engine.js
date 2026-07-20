// engine.js - Pure logic simulation of the C++ matching engine

const INVALID = -1;
const CAPACITY = 1000000;

class OrderNode {
    constructor() {
        this.id = 0;
        this.price = 0;
        this.qty = 0;
        this.side = '';
        this.next = INVALID;
        this.prev = INVALID;
    }
}

class MemoryPool {
    constructor() {
        this.arena = new Array(CAPACITY);
        for (let i = 0; i < CAPACITY; i++) {
            this.arena[i] = new OrderNode();
            this.arena[i].next = i + 1; // Free list chain
        }
        this.arena[CAPACITY - 1].next = INVALID;
        this.nextFree = 0;
        this.activeCount = 0;
    }

    alloc() {
        if (this.nextFree === INVALID) return INVALID; // Out of memory
        const slot = this.nextFree;
        this.nextFree = this.arena[slot].next;
        this.activeCount++;
        return slot;
    }

    free(slot) {
        this.arena[slot].next = this.nextFree;
        this.nextFree = slot;
        this.activeCount--;
    }

    get(slot) {
        return this.arena[slot];
    }
}

class PriceLevel {
    constructor() {
        this.head = INVALID;
        this.tail = INVALID;
        this.totalQty = 0;
    }
}

class Engine {
    constructor(uiCallback) {
        this.pool = new MemoryPool();
        this.bids = new Map(); // price -> PriceLevel (need to sort manually: desc)
        this.asks = new Map(); // price -> PriceLevel (need to sort manually: asc)
        this.orderDir = new Map(); // order_id -> slot
        this.nextOrderId = 1;
        this.uiCallback = uiCallback || (() => {});
    }

    newOrder(side, priceCents, qty) {
        const id = this.nextOrderId++;
        
        let remaining = qty;
        
        if (side === 'B') {
            remaining = this.matchOrder(id, side, priceCents, remaining, this.asks, (a, b) => a - b);
        } else {
            remaining = this.matchOrder(id, side, priceCents, remaining, this.bids, (a, b) => b - a);
        }

        if (remaining > 0) {
            this.addRestingOrder(id, side, priceCents, remaining);
            this.uiCallback({ type: 'ACCEPTED', id, qty: remaining, price: priceCents, side });
        }

        return id;
    }

    matchOrder(incId, side, incPrice, incQty, oppBook, sortFn) {
        let remaining = incQty;
        
        // Get sorted prices
        let prices = Array.from(oppBook.keys()).sort(sortFn);

        for (let price of prices) {
            if (remaining === 0) break;
            
            // Check cross condition
            if ((side === 'B' && incPrice >= price) || (side === 'S' && incPrice <= price)) {
                let level = oppBook.get(price);
                
                while (level.head !== INVALID && remaining > 0) {
                    let restingSlot = level.head;
                    let resting = this.pool.get(restingSlot);
                    
                    let fill = Math.min(remaining, resting.qty);
                    remaining -= fill;
                    resting.qty -= fill;
                    level.totalQty -= fill;
                    
                    this.uiCallback({ 
                        type: 'FILL', 
                        incId, resId: resting.id, 
                        qty: fill, price: price 
                    });

                    if (resting.qty === 0) {
                        let nextSlot = resting.next;
                        if (nextSlot === INVALID) {
                            oppBook.delete(price);
                            level.head = INVALID;
                            level.tail = INVALID;
                        } else {
                            level.head = nextSlot;
                            this.pool.get(nextSlot).prev = INVALID;
                        }
                        this.orderDir.delete(resting.id);
                        this.pool.free(restingSlot);
                    }
                }
            } else {
                break; // Prices no longer cross
            }
        }
        return remaining;
    }

    addRestingOrder(id, side, price, qty) {
        const slot = this.pool.alloc();
        if (slot === INVALID) return;

        let node = this.pool.get(slot);
        node.id = id;
        node.price = price;
        node.qty = qty;
        node.side = side;
        node.next = INVALID;
        node.prev = INVALID;

        this.orderDir.set(id, slot);

        let book = side === 'B' ? this.bids : this.asks;
        if (!book.has(price)) {
            book.set(price, new PriceLevel());
        }
        
        let level = book.get(price);
        level.totalQty += qty;

        if (level.head === INVALID) {
            level.head = slot;
            level.tail = slot;
        } else {
            let tailNode = this.pool.get(level.tail);
            tailNode.next = slot;
            node.prev = level.tail;
            level.tail = slot;
        }
    }

    cancelAll() {
        const ids = Array.from(this.orderDir.keys());
        for (let id of ids) {
            this.cancelOrder(id);
        }
    }

    cancelOrder(id) {
        if (!this.orderDir.has(id)) return;
        
        const slot = this.orderDir.get(id);
        const node = this.pool.get(slot);
        const price = node.price;
        const side = node.side;
        
        let book = side === 'B' ? this.bids : this.asks;
        let level = book.get(price);
        
        level.totalQty -= node.qty;

        if (node.prev !== INVALID) {
            this.pool.get(node.prev).next = node.next;
        } else {
            level.head = node.next;
        }

        if (node.next !== INVALID) {
            this.pool.get(node.next).prev = node.prev;
        } else {
            level.tail = node.prev;
        }

        if (level.head === INVALID) {
            book.delete(price);
        }

        this.orderDir.delete(id);
        this.pool.free(slot);
        
        this.uiCallback({ type: 'CANCELLED', id });
    }

    // --- State Queries for Visualizers ---
    
    getOrderbookSnapshot() {
        let bids = Array.from(this.bids.entries())
            .map(([p, l]) => ({ price: p, qty: l.totalQty }))
            .sort((a, b) => b.price - a.price)
            .slice(0, 10);
            
        let asks = Array.from(this.asks.entries())
            .map(([p, l]) => ({ price: p, qty: l.totalQty }))
            .sort((a, b) => a.price - b.price)
            .slice(0, 10);
            
        return { bids, asks };
    }

    getMemorySnapshot() {
        // Collect active slots (up to 20 for visualizer)
        let usedSlots = Array.from(this.orderDir.values()).slice(0, 20);
        return {
            nextFree: this.pool.nextFree,
            activeCount: this.pool.activeCount,
            capacity: CAPACITY,
            usedSlots
        };
    }

    getNodeSnapshot() {
        let nodes = [];
        let usedSlots = Array.from(this.orderDir.values()).slice(0, 10);
        for (let slot of usedSlots) {
            let n = this.pool.get(slot);
            nodes.push({
                slot,
                id: n.id,
                side: n.side,
                price: n.price,
                qty: n.qty,
                prev: n.prev,
                next: n.next
            });
        }
        return nodes;
    }
}
