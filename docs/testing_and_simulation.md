# Testing and Simulation Tools

The `tests/` directory contains several Python scripts that interact with the matching engine. They serve different purposes: correctness testing, latency testing, order book visualization, and manual trading.

All clients communicate with the engine using a strict 14-byte binary wire protocol over TCP on port `9000`. 

## 1. Client Simulator (`client_simulator.py`)

This script serves two purposes:
1. **Correctness Validation:** It sends a predefined sequence of orders (Buys, Sells, Cancels) and strictly verifies that the engine responds with the correct binary execution reports.
2. **Load Testing:** Using the `--load` flag, it blasts 1,000 binary orders at the engine as fast as possible to populate the engine's internal latency telemetry buffer.

### Logic & Code Snippet
The script packs integers and characters into exact byte lengths using Python's `struct` library. `<cIIIc` means: standard little-endian (`<`), 1 byte char (`c`), three 4-byte integers (`III`), and 1 byte char (`c`). Total: 14 bytes.

```python
import struct

# Format: type(1), order_id(4), price(4), qty(4), side(1) = 14 bytes
REQ_FMT = '<cIIIc'

# Submitting a Buy order for 100 shares at $101.00
# Note: Price is sent as an integer (cents): 10100
req = struct.pack(REQ_FMT, b'N', 1, 10100, 100, b'B')
socket.sendall(req)
```

## 2. Real-Time Ticker Visualizer (`exchange_visualizer.py`)

This script acts like a high-frequency trading bot. It rapidly generates random buy and sell orders around a fluctuating mid-price.
As the matching engine processes these orders and spits out Execution Reports (Fills and Cancels), this script catches them and renders a scrolling, color-coded ticker tape—exactly like the "Time & Sales" feed on a real exchange.

### Logic & Code Snippet
The script runs two threads simultaneously. The main thread sends orders, while a background daemon thread continuously reads exactly 14 bytes from the socket.

```python
def receive_loop(s):
    while True:
        data = s.recv(14)
        if not data: break
        
        # Unpack the binary execution report
        r_type, o_id, f_qty, f_price, status = struct.unpack('<cIIIc', data)
        
        if status.decode() == 'F':
            print(f"FILL {o_id} - {f_qty} shares at ${f_price/100.0:.2f}")
```

## 3. Live Order Book Ladder (`orderbook_visualizer.py`)

Because the matching engine is designed for ultra-low latency, it *does not* broadcast market data to every client on every tick (which would clog the network). Instead, we extended the binary protocol with an **Orderbook Snapshot Query** (`type = 'O'`).

This script continuously polls the engine with this 14-byte query. The engine instantly responds with a 169-byte binary snapshot containing the top 10 bids and top 10 asks directly from its internal memory. The script then unpacks this true global state and renders a live-updating ladder.

### Logic & Code Snippet
```python
# Send a snapshot request
req = struct.pack(REQ_FMT, b'O', 0, 0, 0, b'B')
s.sendall(req)

# Receive 169 bytes (1 byte type + 2 ints + 40 ints for 20 price levels)
data = s.recv(169)
unpacked = struct.unpack('<cII40I', data)
num_bids = unpacked[1]
num_asks = unpacked[2]

# The script iterates over the unpacked integers to build the ladder
for i in range(num_bids):
    price = unpacked[3 + i*2]
    qty = unpacked[4 + i*2]
    # Render Bid...
```

## 4. Manual Trading Client (`manual_client.py`)

This script allows a human operator to interact directly with the engine via an interactive command-line interface. 

### Logic & Example Usage
When you type a command like `buy 100 101.50`, the script automatically assigns an incremental `order_id`, converts `$101.50` to integer cents (`10150`), and packs the binary struct to send to the engine. Fills are printed asynchronously above your input prompt.

**Example Session:**
```text
=== Manual Trading Client ===
Available Commands:
  buy <qty> <price>      e.g. buy 100 101.50
  sell <qty> <price>     e.g. sell 50 102.00
  cancel <order_id>      e.g. cancel 1

Enter command (buy/sell/cancel) > buy 100 100.00
[SYSTEM] Order 1 Accepted
Enter command (buy/sell/cancel) > sell 50 100.00
[SYSTEM] Order 2 Accepted
[FILL] Order 1 filled 50 shares at $100.00
[FILL] Order 2 filled 50 shares at $100.00
```
