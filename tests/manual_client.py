#!/usr/bin/env python3
import socket
import struct
import threading
import sys
import os

REQ_FMT = '<cIIIc'
REP_FMT = '<cIIIc'

# ANSI Color Codes
GREEN = '\033[92m'
RED = '\033[91m'
YELLOW = '\033[93m'
RESET = '\033[0m'
CYAN = '\033[96m'

running = True
next_order_id = 1

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
            
            # Use carriage return and ANSI clearing to print cleanly above the input prompt
            sys.stdout.write('\r\033[K') 
            if status == 'A':
                print(f"[{CYAN}SYSTEM{RESET}] Order {o_id} Accepted")
            elif status == 'F':
                color = GREEN if f_qty > 0 else RED
                print(f"[{color}FILL{RESET}] Order {o_id} filled {f_qty} shares at ${f_price/100.0:.2f}")
            elif status == 'C':
                print(f"[{YELLOW}CANCEL{RESET}] Order {o_id} Cancelled")
                
            sys.stdout.write('Enter command (buy/sell/cancel) > ')
            sys.stdout.flush()
    except Exception:
        pass

def print_help():
    print(f"{CYAN}Available Commands:{RESET}")
    print("  buy <qty> <price>      e.g. buy 100 101.50")
    print("  sell <qty> <price>     e.g. sell 50 102.00")
    print("  cancel <order_id>      e.g. cancel 1")
    print("  exit                   Quit the application")
    print("")

def main():
    global running, next_order_id
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    
    try:
        s.connect(('127.0.0.1', 9000))
    except ConnectionRefusedError:
        print(f"{RED}Error: Matching engine not running on 127.0.0.1:9000{RESET}")
        print("Please start the engine in a separate terminal: ./matching_engine_bin")
        sys.exit(1)

    print(f"{CYAN}=== Manual Trading Client ==={RESET}")
    print_help()

    t = threading.Thread(target=receive_loop, args=(s,), daemon=True)
    t.start()

    try:
        while running:
            try:
                cmd_line = input('Enter command (buy/sell/cancel) > ').strip().lower()
            except EOFError:
                break
                
            if not cmd_line:
                continue
                
            parts = cmd_line.split()
            cmd = parts[0]
            
            if cmd == 'exit' or cmd == 'quit':
                running = False
                break
                
            if cmd in ['buy', 'sell']:
                if len(parts) != 3:
                    print(f"{RED}Error: buy/sell requires qty and price.{RESET}")
                    continue
                try:
                    qty = int(parts[1])
                    price = int(float(parts[2]) * 100) # Convert to cents
                    side = b'B' if cmd == 'buy' else b'S'
                    
                    req = struct.pack(REQ_FMT, b'N', next_order_id, price, qty, side)
                    s.sendall(req)
                    
                    sys.stdout.write(f"\r\033[KSent {cmd.upper()} order {next_order_id}: {qty} @ ${(price/100.0):.2f}\n")
                    next_order_id += 1
                except ValueError:
                    print(f"{RED}Error: qty and price must be numbers.{RESET}")
                    
            elif cmd == 'cancel':
                if len(parts) != 2:
                    print(f"{RED}Error: cancel requires order_id.{RESET}")
                    continue
                try:
                    oid = int(parts[1])
                    req = struct.pack(REQ_FMT, b'C', oid, 0, 0, b'B') # price/qty/side don't matter for cancel
                    s.sendall(req)
                    sys.stdout.write(f"\r\033[KSent CANCEL for order {oid}\n")
                except ValueError:
                    print(f"{RED}Error: order_id must be a number.{RESET}")
            else:
                print(f"{RED}Unknown command.{RESET}")
                print_help()
                
    except KeyboardInterrupt:
        running = False
        
    s.close()
    print("\nDisconnected.")

if __name__ == '__main__':
    main()
