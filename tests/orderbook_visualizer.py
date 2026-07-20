#!/usr/bin/env python3
import socket
import struct
import time
import os
import sys

REQ_FMT = '<cIIIc'
SNAPSHOT_FMT = '<cII40I' # 1 byte char, 2 uint32, 40 uint32 (10 bids * 2 + 10 asks * 2)

# ANSI Color Codes
GREEN = '\033[92m'
RED = '\033[91m'
RESET = '\033[0m'
CYAN = '\033[96m'

def recvall(sock, n):
    data = bytearray()
    while len(data) < n:
        packet = sock.recv(n - len(data))
        if not packet:
            return None
        data.extend(packet)
    return bytes(data)

def fetch_and_render(s):
    # Send snapshot request
    req = struct.pack(REQ_FMT, b'O', 0, 0, 0, b'B')
    s.sendall(req)
    
    data = recvall(s, 169)
    if data is None:
        return
        
    unpacked = struct.unpack(SNAPSHOT_FMT, data)
    num_bids = unpacked[1]
    num_asks = unpacked[2]
    
    bids = []
    for i in range(num_bids):
        idx = 3 + i * 2
        bids.append((unpacked[idx], unpacked[idx+1]))
        
    asks = []
    for i in range(num_asks):
        idx = 3 + (10 + i) * 2
        asks.append((unpacked[idx], unpacked[idx+1]))
        
    os.system('clear' if os.name == 'posix' else 'cls')
    print(f"{CYAN}{'='*40}{RESET}")
    print(f"{CYAN}       TRUE ORDER BOOK LADDER         {RESET}")
    print(f"{CYAN}{'='*40}{RESET}")
    print(f"{'BID QTY':<10} {'PRICE':<18} {'ASK QTY':>10}")
    print("-" * 40)
    
    # Asks come sorted lowest-first from engine. We want highest-first visually so best ask is at bottom.
    sorted_asks = list(reversed(asks))
    for price, qty in sorted_asks:
        print(f"{'':<10} {RED}${price/100.0:<17.2f}{RESET} {RED}{qty:>10}{RESET}")
        
    print("-" * 40) # The Spread
    
    # Bids come sorted highest-first from engine. Perfect.
    for price, qty in bids:
        print(f"{GREEN}{qty:<10}{RESET} {GREEN}${price/100.0:<17.2f}{RESET} {'':>10}")
        
    print(f"\n{CYAN}Live querying engine... Press Ctrl+C to stop.{RESET}")

def main():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    
    try:
        s.connect(('127.0.0.1', 9000))
    except ConnectionRefusedError:
        print(f"{RED}Error: Matching engine not running on 127.0.0.1:9000{RESET}")
        sys.exit(1)

    try:
        while True:
            fetch_and_render(s)
            time.sleep(0.2)
    except KeyboardInterrupt:
        os.system('clear' if os.name == 'posix' else 'cls')
        print("Simulation stopped.")
        s.close()

if __name__ == '__main__':
    main()
