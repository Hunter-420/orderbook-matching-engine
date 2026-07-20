#!/usr/bin/env python3
import socket
import struct
import time
import random
import threading
import sys
import os

REQ_FMT = '<cIIIc'
REP_FMT = '<cIIIc'

# ANSI Color Codes
GREEN = '\033[92m'
RED = '\033[91m'
RESET = '\033[0m'
CYAN = '\033[96m'

# Local state tracking
live_orders = {} # order_id -> {'side': 'B'/'S', 'price': int, 'qty': int}
state_lock = threading.Lock()
running = True

def render_orderbook():
    # Group by price
    bids = {}
    asks = {}
    
    with state_lock:
        for oid, order in live_orders.items():
            if order['qty'] <= 0:
                continue
            if order['side'] == b'B':
                bids[order['price']] = bids.get(order['price'], 0) + order['qty']
            else:
                asks[order['price']] = asks.get(order['price'], 0) + order['qty']
                
    os.system('clear' if os.name == 'posix' else 'cls')
    print(f"{CYAN}{'='*40}{RESET}")
    print(f"{CYAN}       LIVE ORDER BOOK LADDER         {RESET}")
    print(f"{CYAN}{'='*40}{RESET}")
    print(f"{'BID QTY':<10} {'PRICE':<18} {'ASK QTY':>10}")
    print("-" * 40)
    
    # Sort asks descending so lowest ask is at the bottom of the top half
    sorted_asks = sorted(asks.keys(), reverse=True)
    for p in sorted_asks:
        print(f"{'':<10} {RED}${p/100.0:<17.2f}{RESET} {RED}{asks[p]:>10}{RESET}")
        
    print("-" * 40) # The Spread
    
    # Sort bids descending so highest bid is at the top of the bottom half
    sorted_bids = sorted(bids.keys(), reverse=True)
    for p in sorted_bids:
        print(f"{GREEN}{bids[p]:<10}{RESET} {GREEN}${p/100.0:<17.2f}{RESET} {'':>10}")
        
    print(f"\n{CYAN}Press Ctrl+C to stop simulation.{RESET}")

def receive_loop(s):
    global running
    try:
        while running:
            data = s.recv(14)
            if not data:
                break
            if len(data) != 14:
                continue
                
            r_type, o_id, f_qty, f_price, status_byte = struct.unpack(REP_FMT, data)
            status = status_byte.decode()
            
            with state_lock:
                if status == 'F': # Fill
                    if o_id in live_orders:
                        live_orders[o_id]['qty'] -= f_qty
                        if live_orders[o_id]['qty'] <= 0:
                            del live_orders[o_id]
                elif status == 'C': # Cancelled
                    if o_id in live_orders:
                        del live_orders[o_id]
                        
            render_orderbook()
    except Exception:
        pass

def main():
    global running
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    
    try:
        s.connect(('127.0.0.1', 9000))
    except ConnectionRefusedError:
        print(f"{RED}Error: Matching engine not running on 127.0.0.1:9000{RESET}")
        print("Please start the engine in a separate terminal: ./matching_engine_bin")
        sys.exit(1)

    t = threading.Thread(target=receive_loop, args=(s,), daemon=True)
    t.start()

    mid_price = 10000 # $100.00
    order_id = 1

    try:
        while running:
            # Simulate a market maker submitting random depth
            is_buy = random.choice([True, False])
            side = b'B' if is_buy else b'S'
            
            # Distance from mid price (1 to 20 cents)
            spread_dist = random.randint(1, 20)
            price = mid_price - spread_dist if is_buy else mid_price + spread_dist
            qty = random.randint(1, 5) * 10
            
            with state_lock:
                live_orders[order_id] = {'side': side, 'price': price, 'qty': qty}
                
            req = struct.pack(REQ_FMT, b'N', order_id, price, qty, side)
            s.sendall(req)
            order_id += 1
            
            # Occasionally submit a market-taking order that crosses the spread
            if random.random() < 0.2:
                cross_buy = random.choice([True, False])
                cross_side = b'B' if cross_buy else b'S'
                cross_price = mid_price + 10 if cross_buy else mid_price - 10
                cross_qty = random.randint(1, 5) * 10
                
                with state_lock:
                    live_orders[order_id] = {'side': cross_side, 'price': cross_price, 'qty': cross_qty}
                    
                req = struct.pack(REQ_FMT, b'N', order_id, cross_price, cross_qty, cross_side)
                s.sendall(req)
                order_id += 1
                
                # Drift mid price slightly
                mid_price += random.randint(-2, 2)
                
            render_orderbook()
            time.sleep(random.uniform(0.1, 0.4))
            
            # Memory safety for the visualizer, periodically clear old far-out orders
            with state_lock:
                if len(live_orders) > 100:
                    old_ids = list(live_orders.keys())[:20]
                    for oid in old_ids:
                        req = struct.pack(REQ_FMT, b'C', oid, 0, 0, b'B')
                        s.sendall(req)
            
    except KeyboardInterrupt:
        running = False
        s.close()
        os.system('clear' if os.name == 'posix' else 'cls')
        print("Simulation stopped.")

if __name__ == '__main__':
    main()
