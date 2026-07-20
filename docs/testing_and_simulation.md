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

Because the matching engine is designed for ultra-low latency, it *does not* broadcast its internal state. It only broadcasts executions. 
To build an order book ladder, this script maintains its own local copy of the market. It tracks every order it submits. When it receives a fill, it reduces the quantity. When it receives a cancel, it deletes the order. It then aggregates the remaining orders by price and renders an interactive, live-updating ladder.

### Logic & Code Snippet
```python
# Maintain a local state of all live orders
live_orders = {
    # order_id: {'side': b'B', 'price': 10100, 'qty': 100}
}

# When an execution report arrives:
if status == 'F': # Fill
    live_orders[order_id]['qty'] -= filled_qty
    if live_orders[order_id]['qty'] <= 0:
        del live_orders[order_id] # Order fully filled, remove from book

# Rendering the ladder by aggregating quantities at each price level
bids = {}
for order in live_orders.values():
    if order['side'] == b'B':
        bids[order['price']] = bids.get(order['price'], 0) + order['qty']
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
