#!/usr/bin/env python3
"""
node_visualizer.py

Connects to the matching engine and polls for a live snapshot of every
active OrderNode currently resting in the engine's MemoryPool.

Each OrderNode represents one resting limit order in the book. The visualizer
shows the raw struct fields exactly as they exist in C++ memory: the physical
pool slot index, the order id, price, quantity, side, and the prev/next linked
list pointers that chain orders together at the same price level.

The engine is the single source of truth. This script never modifies any state.
It only sends a 14-byte query and reads back a 289-byte binary response, then
renders it to the terminal.

Run this in a dedicated terminal alongside the engine and the other visualizers.
"""

import socket
import struct
import time
import os
import sys

# The standard 14-byte request packet format used across all query types
REQ_FMT = '<cIIIc'

# NodeSnapshot wire format (289 bytes total):
#   1 byte  : type char ('D')
#   4 bytes : total_active (uint32)
#   4 bytes : num_nodes    (uint32)
#  28 bytes : each NodeData (slot_index, order_id, price, quantity, side, pad[3], next_idx, prev_idx)
#   NodeData format: 5 uint32 fields + 1 char + 3 pad + 2 uint32 = 4*5 + 1 + 3 + 4*2 = 32? 
# Let's unpack exactly: 
#   slot_index(4) order_id(4) price(4) quantity(4) side(1) pad(3) next_idx(4) prev_idx(4) = 28 bytes per node
NODE_DATA_FMT = '<IIIIc3sII'  # 28 bytes per node
NODE_DATA_SIZE = struct.calcsize(NODE_DATA_FMT)  # should be 28

SNAPSHOT_HEADER_FMT = '<cII'  # type(1) + total_active(4) + num_nodes(4) = 9 bytes
SNAPSHOT_HEADER_SIZE = struct.calcsize(SNAPSHOT_HEADER_FMT)

NODE_SNAPSHOT_SIZE = SNAPSHOT_HEADER_SIZE + NODE_DATA_SIZE * 10  # 9 + 280 = 289 bytes

INVALID = 0xFFFFFFFF  # UINT32_MAX sentinel meaning "no node"

GREEN  = '\033[92m'
RED    = '\033[91m'
CYAN   = '\033[96m'
YELLOW = '\033[93m'
BOLD   = '\033[1m'
RESET  = '\033[0m'


def recvall(sock, n):
    """Read exactly n bytes from the socket, reassembling TCP fragments."""
    data = bytearray()
    while len(data) < n:
        packet = sock.recv(n - len(data))
        if not packet:
            return None
        data.extend(packet)
    return bytes(data)


def fmt_idx(idx):
    """Display UINT32_MAX as 'NONE' and any valid index as its number."""
    return 'NONE' if idx == INVALID else str(idx)


def fetch_and_render(s):
    req = struct.pack(REQ_FMT, b'D', 0, 0, 0, b'B')
    s.sendall(req)

    data = recvall(s, NODE_SNAPSHOT_SIZE)
    if data is None:
        return

    msg_type, total_active, num_nodes = struct.unpack_from(SNAPSHOT_HEADER_FMT, data, 0)
    offset = SNAPSHOT_HEADER_SIZE

    nodes = []
    for _ in range(num_nodes):
        slot_idx, order_id, price, qty, side_b, _pad, next_idx, prev_idx = \
            struct.unpack_from(NODE_DATA_FMT, data, offset)
        side = side_b.decode('ascii')
        nodes.append((slot_idx, order_id, price, qty, side, next_idx, prev_idx))
        offset += NODE_DATA_SIZE

    os.system('clear' if os.name == 'posix' else 'cls')
    print(f"{CYAN}{BOLD}{'=' * 72}{RESET}")
    print(f"{CYAN}{BOLD}          LIVE NODE DATA SNAPSHOT  (C++ MemoryPool)            {RESET}")
    print(f"{CYAN}{BOLD}{'=' * 72}{RESET}")
    print(f"  Total active resting orders in engine: {YELLOW}{total_active}{RESET}")
    print(f"  Showing first {num_nodes} node(s)\n")

    if num_nodes == 0:
        print(f"  {YELLOW}No active orders. Place an order using manual_client.py.{RESET}")
    else:
        # Table header
        hdr = (f"  {'Slot':>6}  {'OrdID':>6}  {'Side':>4}  "
               f"{'Price':>10}  {'Qty':>8}  {'Prev Slot':>10}  {'Next Slot':>10}")
        print(hdr)
        print("  " + "-" * 68)

        for slot_idx, order_id, price, qty, side, next_idx, prev_idx in nodes:
            color = GREEN if side == 'B' else RED
            label = "BID" if side == 'B' else "ASK"
            prev_s = fmt_idx(prev_idx)
            next_s = fmt_idx(next_idx)
            print(
                f"  {color}[{slot_idx:>4}]{RESET}  "
                f"{order_id:>6}  {color}{label}{RESET}  "
                f"${price/100.0:>9.2f}  "
                f"{qty:>8}  "
                f"{prev_s:>10}  {next_s:>10}"
            )

        # If multiple nodes share a price, draw the linked list chain explicitly
        price_groups = {}
        for row in nodes:
            p = row[2]
            price_groups.setdefault(p, []).append(row)

        chains = {p: rows for p, rows in price_groups.items() if len(rows) > 1}
        if chains:
            print(f"\n  {CYAN}Linked List Chains at Shared Price Levels:{RESET}")
            for price, rows in chains.items():
                chain_str = " <-> ".join(f"[slot {r[0]} ord {r[1]}]" for r in rows)
                print(f"    ${price/100.0:.2f}: {chain_str}")

    print(f"\n{CYAN}Polling engine every 500ms. Press Ctrl+C to stop.{RESET}")


def main():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

    try:
        s.connect(('127.0.0.1', 9000))
    except ConnectionRefusedError:
        print(f"{RED}Error: Matching engine is not running on 127.0.0.1:9000{RESET}")
        print("Start it first with: ./matching_engine_bin")
        sys.exit(1)

    try:
        while True:
            fetch_and_render(s)
            time.sleep(0.5)
    except KeyboardInterrupt:
        os.system('clear' if os.name == 'posix' else 'cls')
        print("Node visualizer stopped.")
    finally:
        s.close()


if __name__ == '__main__':
    main()
