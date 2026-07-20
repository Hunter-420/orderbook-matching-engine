# Testing and Simulation Tools

The matching engine is a self-contained C++ server process that holds all order state in its internal memory. It listens on TCP port 9000 and speaks a strict 14-byte binary protocol. Every tool in the `tests/` directory is a Python client that connects to that server and communicates with it through that same protocol.

None of the visualizers generate fake data or simulate orders on their own. They send query packets to the running engine and receive binary snapshots of its actual internal C++ state in response. The engine is always the single source of truth.

## How the Query System Works

When you send a new limit order (`type = 'N'`), the engine routes it into the matching logic. When you send a query packet (`type = 'O'`, `'M'`, or `'D'`), the engine intercepts it inside the `epoll` event loop before it ever reaches the matching code. It packages the requested internal state into a binary struct and immediately sends it back. The hot path is never touched.

This is the key architectural guarantee: visualizing the engine has zero latency cost on trading.

```
Python Client                        C++ Matching Engine
    |                                        |
    | -- 14-byte request ('O','M','D') -->   |
    |                                        |  [epoll intercepts query]
    |                                        |  [reads internal state]
    |                                        |  [packs binary struct]
    | <-- N-byte binary snapshot ----------- |
    |                                        |
    |  [renders to terminal]                 |  [matching continues unaffected]
```

## Tools Reference

### 1. Client Simulator (`client_simulator.py`)

This script serves two purposes. In its default mode it sends a predefined sequence of orders and strictly verifies that the engine responds with the correct execution reports. With the `--load` flag it fires a thousand orders as fast as possible to fill the engine's internal latency telemetry ring buffer.

The script packs every message using Python's `struct` library with the `<cIIIc` format, which means little-endian byte order, one character, three four-byte unsigned integers, and one trailing character. The result is always exactly 14 bytes.

```python
import struct

REQ_FMT = '<cIIIc'

# Submit a buy order for 100 shares at $101.00
# Price is always sent as integer cents to avoid floating point drift
req = struct.pack(REQ_FMT, b'N', 1, 10100, 100, b'B')
socket.sendall(req)
```

### 2. Real-Time Ticker Visualizer (`exchange_visualizer.py`)

This script runs two threads. The main thread continuously generates random orders around a drifting mid price and submits them. A background daemon thread reads 14-byte execution reports back from the engine and prints them as a scrolling tape.

The randomized orders create a natural flow of fills, which the ticker renders in real time with color coded direction. This is the tool to run if you want to see the engine handling continuous high-frequency activity.

```python
def receive_loop(s):
    while True:
        data = s.recv(14)
        if not data: break
        r_type, o_id, f_qty, f_price, status = struct.unpack('<cIIIc', data)
        if status.decode() == 'F':
            print(f"FILL {o_id}  {f_qty} shares at ${f_price/100.0:.2f}")
```

### 3. Live Order Book Ladder (`orderbook_visualizer.py`)

This visualizer polls the engine with the `'O'` query type every 200 milliseconds. The engine responds with a 169-byte `OrderbookSnapshot` containing the top ten bid price levels and top ten ask price levels with their aggregated quantities.

Because the engine is the source, this ladder reflects every order from every connected client simultaneously. If you type a buy order in `manual_client.py`, it will appear on this ladder within 200 milliseconds regardless of which Python process placed it.

```python
REQ_FMT = '<cIIIc'

req = struct.pack(REQ_FMT, b'O', 0, 0, 0, b'B')
s.sendall(req)

data = recvall(s, 169)
unpacked = struct.unpack('<cII40I', data)
num_bids = unpacked[1]
num_asks = unpacked[2]

for i in range(num_bids):
    price = unpacked[3 + i * 2]
    qty   = unpacked[4 + i * 2]
    # render bid row
```

### 4. Memory Layout Visualizer (`memory_visualizer.py`)

This visualizer polls the engine with the `'M'` query type every 500 milliseconds. The engine responds with a 49-byte `MemoryStateSnapshot` that contains the `MemoryPool`'s current free-list head index, the count of active orders, and the pool slot indices of the first ten live nodes.

Watching this alongside `manual_client.py` shows the zero-allocation guarantee in action. Each new order moves the free-list head one position forward. Each cancel or fill moves it back. The heap is never touched after startup.

```python
REQ_FMT = '<cIIIc'

req = struct.pack(REQ_FMT, b'M', 0, 0, 0, b'B')
s.sendall(req)

data = recvall(s, 49)
unpacked = struct.unpack('<cII10I', data)
next_free_idx = unpacked[1]
total_active  = unpacked[2]
```

### 5. Node Data Visualizer (`node_visualizer.py`)

This is the most detailed of the visualizers. It polls the engine with the `'D'` query type every 500 milliseconds. The engine responds with a 289-byte `NodeSnapshot` containing the full raw struct data for up to the first ten active `OrderNode` objects currently sitting in the memory pool.

Each `NodeData` entry carries every field of the original C++ struct: the physical pool slot index, the order id, the price in integer cents, the remaining quantity, the side, and the `prev_idx` and `next_idx` linked-list pointers. When two orders rest at the same price level, you can see their `prev` and `next` pointers linking them together directly on screen.

```python
NODE_DATA_FMT = '<IIIIc3sII'  # 28 bytes per node
HDR_FMT       = '<cII'        # 9 bytes header

req = struct.pack(REQ_FMT, b'D', 0, 0, 0, b'B')
s.sendall(req)

data = recvall(s, 289)

msg_type, total_active, num_nodes = struct.unpack_from(HDR_FMT, data, 0)
offset = 9
for i in range(num_nodes):
    slot, oid, price, qty, side_b, _pad, nxt, prv = struct.unpack_from(NODE_DATA_FMT, data, offset)
    offset += 28
```

**Example output when two buy orders are resting at the same price:**

```
  Slot  OrdID  Side       Price       Qty   Prev Slot  Next Slot
  ----------------------------------------------------------------
  [  0]      1   BID    $100.00       100        NONE          1
  [  1]      2   BID    $100.00        50           0       NONE
  [  2]      3   ASK    $101.00        30        NONE       NONE

  Linked List Chains at Shared Price Levels:
    $100.00: [slot 0 ord 1] <-> [slot 1 ord 2]
```

### 6. Manual Trading Client (`manual_client.py`)

An interactive command-line tool for placing orders manually into the live engine. The receive thread runs in the background and prints execution reports asynchronously above your input prompt.

```text
Enter command (buy/sell/cancel) > buy 100 100.00
Sent BUY order 1: 100 @ $100.00
[SYSTEM] Order 1 Accepted
Enter command (buy/sell/cancel) > sell 50 100.00
[FILL] Order 1 filled 50 shares at $100.00
```
