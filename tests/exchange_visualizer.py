#!/usr/bin/env python3
import socket
import struct
import time
import random
import threading
import sys

# Fixed-width protocol formats
REQ_FMT = '<cIIIc'
REP_FMT = '<cIIIc'

# ANSI Color Codes
GREEN = '\033[92m'
RED = '\033[91m'
YELLOW = '\033[93m'
CYAN = '\033[96m'
RESET = '\033[0m'

def print_header():
    print(f"{CYAN}{'='*60}{RESET}")
    print(f"{CYAN}       REAL-TIME EXCHANGE TAPE SIMULATOR      {RESET}")
    print(f"{CYAN}{'='*60}{RESET}")
    print(f"{'TYPE':<8} {'ORDER_ID':<10} {'SIDE':<6} {'QTY':<8} {'PRICE':<10} {'STATUS'}")
    print("-" * 60)

def display_report(data):
    if len(data) != 14:
        return
    r_type, o_id, f_qty, f_price, status_byte = struct.unpack(REP_FMT, data)
    status = status_byte.decode()
    
    if status == 'A':
        # Don't clutter with ACKs, just show fills
        return 
        
    if status == 'F':
        color = GREEN if f_qty > 0 else RED # Just assigning a color for visuals
        print(f"{color}{'FILL':<8} {o_id:<10} {'-':<6} {f_qty:<8} ${f_price/100.0:<9.2f} MATCHED{RESET}")
    elif status == 'C':
        print(f"{YELLOW}{'CANCEL':<8} {o_id:<10} {'-':<6} {'-':<8} {'-':<10} CANCELLED{RESET}")

def receive_loop(s):
    try:
        while True:
            data = s.recv(14)
            if not data:
                break
            display_report(data)
    except Exception as e:
        print(f"Connection closed: {e}")

def main():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    
    try:
        s.connect(('127.0.0.1', 9000))
    except ConnectionRefusedError:
        print(f"{RED}Error: Matching engine not running on 127.0.0.1:9000{RESET}")
        print("Please start the engine first: ./matching_engine_bin")
        sys.exit(1)

    print_header()

    # Start a background thread to listen for fills
    t = threading.Thread(target=receive_loop, args=(s,), daemon=True)
    t.start()

    # Simulate random market maker and taker flow
    mid_price = 10000 # $100.00
    order_id = 1

    try:
        while True:
            # Randomly decide to buy or sell
            is_buy = random.choice([True, False])
            side = b'B' if is_buy else b'S'
            
            # Fluctuate price slightly around mid-price
            price_offset = random.randint(-50, 50) # +/- $0.50
            price = mid_price + price_offset
            
            qty = random.randint(1, 10) * 10
            
            # Print intent
            side_str = f"{GREEN}BUY{RESET}" if is_buy else f"{RED}SELL{RESET}"
            print(f"SUBMIT   {order_id:<10} {side_str:<15} {qty:<8} ${price/100.0:<9.2f} PENDING")
            
            req = struct.pack(REQ_FMT, b'N', order_id, price, qty, side)
            s.sendall(req)
            
            order_id += 1
            
            # Random drift in mid-price to simulate market movement
            mid_price += random.randint(-10, 10)
            
            time.sleep(random.uniform(0.1, 0.5))
            
    except KeyboardInterrupt:
        print(f"\n{CYAN}Simulation stopped by user.{RESET}")
        s.close()

if __name__ == '__main__':
    main()
