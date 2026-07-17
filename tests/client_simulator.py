#!/usr/bin/env python3
import socket
import struct
import time

# OrderRequest: type(1), order_id(4), price(4), qty(4), side(1) = 14 bytes
REQ_FMT = '<cIIIc'

# ExecutionReport: type(1), order_id(4), filled_qty(4), fill_price(4), status(1) = 14 bytes
REP_FMT = '<cIIIc'

def print_report(data):
    if len(data) != 14:
        print(f"Invalid report size: {len(data)}")
        return
    r_type, o_id, f_qty, f_price, status = struct.unpack(REP_FMT, data)
    print(f"Report -> type:{r_type.decode()}, order_id:{o_id}, filled_qty:{f_qty}, fill_price:{f_price/100.0 if f_price > 0 else 0}, status:{status.decode()}")

def main():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    s.connect(('127.0.0.1', 9000))

    # Test 1: Submit a sell order
    print("--- Submitting Sell 100 @ $101.00 ---")
    req1 = struct.pack(REQ_FMT, b'N', 1, 10100, 100, b'S')
    s.sendall(req1)
    print_report(s.recv(14)) # Wait for ACK

    # Test 2: Submit a matching buy order (basic match)
    print("--- Submitting Buy 100 @ $101.00 ---")
    req2 = struct.pack(REQ_FMT, b'N', 2, 10100, 100, b'B')
    s.sendall(req2)
    print_report(s.recv(14)) # Wait for ACK
    print_report(s.recv(14)) # Wait for Fill (Buy)
    print_report(s.recv(14)) # Wait for Fill (Sell)

    # Test 3: Cancellation
    print("--- Submitting Buy 50 @ $99.00 ---")
    req3 = struct.pack(REQ_FMT, b'N', 3, 9900, 50, b'B')
    s.sendall(req3)
    print_report(s.recv(14)) # Wait for ACK

    print("--- Cancelling Order 3 ---")
    req4 = struct.pack(REQ_FMT, b'C', 3, 0, 0, b'B') # side doesn't matter for cancel, price/qty 0
    s.sendall(req4)
    print_report(s.recv(14)) # Wait for Cancel ACK

    # Submitting matching sell to ensure it was cancelled
    print("--- Submitting Sell 50 @ $99.00 ---")
    req5 = struct.pack(REQ_FMT, b'N', 4, 9900, 50, b'S')
    s.sendall(req5)
    print_report(s.recv(14)) # Wait for ACK (No fills should come)

    print("All tests completed.")
    s.close()

if __name__ == '__main__':
    main()
