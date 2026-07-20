#!/usr/bin/env python3
import socket
import struct
import time
import os
import sys

REQ_FMT = '<cIIIc'
MEM_FMT = '<cII10I' # 1 char, 2 uint32, 10 uint32 (49 bytes)

# ANSI Color Codes
CYAN = '\033[96m'
GREEN = '\033[92m'
YELLOW = '\033[93m'
RESET = '\033[0m'

def fetch_and_render(s):
    req = struct.pack(REQ_FMT, b'M', 0, 0, 0, b'B')
    s.sendall(req)
    
    data = s.recv(49)
    if len(data) != 49:
        return
        
    unpacked = struct.unpack(MEM_FMT, data)
    next_free_idx = unpacked[1]
    total_active = unpacked[2]
    
    os.system('clear' if os.name == 'posix' else 'cls')
    print(f"{CYAN}{'='*50}{RESET}")
    print(f"{CYAN}       INTERNAL MEMORY LAYOUT (C++)           {RESET}")
    print(f"{CYAN}{'='*50}{RESET}")
    
    print(f"Total Arena Capacity:      {1_000_000:,} slots")
    print(f"Active Live Orders:        {YELLOW}{total_active:,}{RESET} slots")
    print(f"Available Free Slots:      {GREEN}{1_000_000 - total_active:,}{RESET} slots")
    
    print(f"\n{CYAN}--- Pointers ---{RESET}")
    print(f"Free List Head Index:      -> [{next_free_idx}]")
    
    print(f"\n{CYAN}--- Top Physical Slots in Use (Order Directory) ---{RESET}")
    if total_active == 0:
        print("  (Empty)")
    else:
        for i in range(min(10, total_active)):
            slot = unpacked[3 + i]
            print(f"  Slot Index: [{slot}]")
            
    print(f"\n{CYAN}Live querying engine... Press Ctrl+C to stop.{RESET}")

def main():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    
    try:
        s.connect(('127.0.0.1', 9000))
    except ConnectionRefusedError:
        print(f"Error: Matching engine not running on 127.0.0.1:9000")
        sys.exit(1)

    try:
        while True:
            fetch_and_render(s)
            time.sleep(0.5)
    except KeyboardInterrupt:
        os.system('clear' if os.name == 'posix' else 'cls')
        print("Simulation stopped.")
        s.close()

if __name__ == '__main__':
    main()
