#!/usr/bin/env python3
import socket
import struct
import time
import sys

REQ_FMT = '<cIIIc'
REP_FMT = '<cIIIc'

def print_report(data):
    if len(data) != 14:
        return
    r_type, o_id, f_qty, f_price, status = struct.unpack(REP_FMT, data)
    print(f"Report -> type:{r_type.decode()}, order_id:{o_id}, filled_qty:{f_qty}, fill_price:{f_price/100.0 if f_price > 0 else 0}, status:{status.decode()}")

def main():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    s.connect(('127.0.0.1', 9000))

    if len(sys.argv) > 1 and sys.argv[1] == '--load':
        # Load test mode: 1000 orders to measure latency
        print("--- Running Load Test (1000 orders) ---")
        start = time.time()
        for i in range(1, 1001):
            req = struct.pack(REQ_FMT, b'N', i, 10000 + (i % 100), 10, b'B' if i % 2 == 0 else b'S')
            s.sendall(req)
            # We don't print them all to avoid IO overhead polluting latency,
            # but we read the ACK (and potential fills) so the socket doesn't buffer fully.
            # Simplified for load testing: just read 14 bytes per loop
            s.recv(14)
        end = time.time()
        print(f"Load test completed in {end - start:.4f} seconds.")
    else:
        # Standard validation scenarios
        # Test 1: Submit a sell order
        print("--- Submitting Sell 100 @ $101.00 ---")
        req1 = struct.pack(REQ_FMT, b'N', 1, 10100, 100, b'S')
        s.sendall(req1)
        print_report(s.recv(14))

        # Test 2: Submit a matching buy order (basic match)
        print("--- Submitting Buy 100 @ $101.00 ---")
        req2 = struct.pack(REQ_FMT, b'N', 2, 10100, 100, b'B')
        s.sendall(req2)
        print_report(s.recv(14)) 
        print_report(s.recv(14)) 
        print_report(s.recv(14)) 

        # Test 3: Cancellation
        print("--- Submitting Buy 50 @ $99.00 ---")
        req3 = struct.pack(REQ_FMT, b'N', 3, 9900, 50, b'B')
        s.sendall(req3)
        print_report(s.recv(14))

        print("--- Cancelling Order 3 ---")
        req4 = struct.pack(REQ_FMT, b'C', 3, 0, 0, b'B')
        s.sendall(req4)
        print_report(s.recv(14))

        print("--- Submitting Sell 50 @ $99.00 ---")
        req5 = struct.pack(REQ_FMT, b'N', 4, 9900, 50, b'S')
        s.sendall(req5)
        print_report(s.recv(14))

        print("All correctness tests completed.")

    s.close()

if __name__ == '__main__':
    main()
