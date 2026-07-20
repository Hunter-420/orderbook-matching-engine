# Phase 4: Hardware Isolation and Latency Measurement

## What This Phase Does

Phase 4 deals with two problems that only appear when you run the engine on real hardware under real load. The first problem is latency jitter: the engine can sometimes take 10x longer to process an order than usual, not because the matching logic is slow, but because the operating system interrupted the process and moved it to a different CPU core. The second problem is measurement: I want to know exactly how fast the engine is, measured in nanoseconds, without the act of measurement itself slowing anything down.

Phase 4 solves both.

## Problem 1: CPU Core Hopping and Cache Invalidation

The Linux scheduler is designed to balance workloads across all available CPU cores. It regularly moves threads from one core to another to keep every core equally busy. This is great for general-purpose workloads but devastating for a latency-sensitive engine.

Every CPU core has its own L1 and L2 cache, which are small ultra-fast memory banks that hold copies of recently accessed data. When the engine runs on Core 1, its order book data — the bid map, the ask map, the memory pool arena — all live in Core 1's L1 cache. Accessing that data takes about 1 nanosecond.

When the scheduler moves the engine to Core 3, Core 3's cache has no idea what the order book looks like. Every memory access must go all the way to main RAM, which takes 60 to 100 nanoseconds per read. For the duration of that migration, every order the engine processes takes tens of thousands of nanoseconds instead of a few hundred. This appears in the latency data as extreme tail spikes.

The solution is CPU affinity pinning. I tell the Linux kernel that this specific thread must always run on Core 1 and only Core 1. The kernel respects this instruction. The cache stays warm forever.

```cpp
// src/main.cpp
void pin_to_core(int core_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);           // Clear all cores from the allowed set
    CPU_SET(core_id, &cpuset);   // Add only core_id to the allowed set

    pthread_t current_thread = pthread_self();
    pthread_setaffinity_np(current_thread, sizeof(cpu_set_t), &cpuset);
    // From now on the OS will never schedule this thread off core_id
}

// In main():
pin_to_core(1);  // Always run on CPU core 1
```

## Problem 2: Measuring Nanosecond Latency Without Disrupting It

To understand the engine's performance I record the latency of every single order: the time from when the order bytes arrived to when the execution report bytes were sent back. That sounds simple, but the naive approach destroys the measurement.

If you call `std::cout` to print the latency after each order, `std::cout` flushes a buffer, which can involve a system call, which can take 10,000 nanoseconds. You would be measuring the cost of printing, not the cost of matching.

The solution is to never print during trading. Instead I maintain a pre-allocated ring buffer called `TelemetryBuffer` that holds up to one million nanosecond samples. Writing a sample is a single array assignment and an index increment. No system call, no allocation, no lock. It takes under one nanosecond.

When the user presses Ctrl+C to shut down the engine, I sort the buffer and compute percentiles. The engine is already stopped at that point so I can take as long as I want.

## How Measurement Works in Practice

The engine starts and pins itself to Core 1. The telemetry buffer is pre-allocated with space for one million samples.

Order 1 arrives. I immediately capture a start timestamp using `high_resolution_clock::now()`. I run the matching logic, send the execution report, then capture an end timestamp. The difference is 2,500 nanoseconds. I write `2500` into `TelemetryBuffer[0]` and increment the index to 1.

Order 2 arrives. Same process, difference is 3,100 nanoseconds. I write `3100` into `TelemetryBuffer[1]`. Index becomes 2.

This continues for 1,000 orders. The user presses Ctrl+C.

The SIGINT handler sets `keep_running = false`. The event loop exits on its next iteration. `telemetry.dump_percentiles()` is called. It copies the 1,000 samples into a vector, sorts it, and computes the percentiles using direct index arithmetic.

```
p50:  3012 ns      (the median — half of all orders were faster than this)
p90:  14638 ns     (90% of orders were faster than this)
p99:  24958 ns     (99% of orders were faster — 10 in 1000 hit this tail)
p99.9: 84334 ns    (the worst 1 in 1000 orders — almost certainly a cache miss from a scheduler interrupt)
```

## Code: The Telemetry Buffer

```cpp
// include/telemetry.hpp
class TelemetryBuffer {
public:
    static constexpr size_t CAPACITY = 1'000'000;

    void record(int64_t ns) {
        samples_[index_] = ns;   // One array write
        index_++;
        if (index_ >= CAPACITY) {
            index_ = 0;          // Wrap around: oldest sample gets overwritten
            wrapped_ = true;
        }
    }

    void dump_percentiles() const;  // Only called at shutdown

private:
    std::array<int64_t, CAPACITY> samples_;  // Pre-allocated at object creation
    size_t index_  = 0;
    bool   wrapped_ = false;
};
```

The `samples_` array is a member of the class. It is allocated when the `TelemetryBuffer` object is constructed at program startup. During trading, `record()` never touches the heap.

## Code: Nanosecond Timestamps in the Event Loop

I capture timestamps as tightly as possible around the actual work to exclude waiting time.

```cpp
// src/main.cpp — inside the epoll loop, after bytes arrive
if (bytes == sizeof(OrderRequest)) {
    // Timestamp 1: bytes have arrived in user space, start the clock
    auto t_start = std::chrono::high_resolution_clock::now();

    OrderRequest req;
    std::memcpy(&req, buf, sizeof(OrderRequest));

    if (req.type == 'N') {
        engine.new_order(req.order_id, req.side, req.price, req.quantity);
        ExecutionReport ack{'E', req.order_id, 0, 0, 'A'};
        send(fd, &ack, sizeof(ack), 0);
    }

    auto fills = engine.take_fills();
    for (const auto& fill : fills) {
        ExecutionReport report{'E', fill.buy_order_id, fill.quantity, fill.price, 'F'};
        send(fd, &report, sizeof(report), 0);
    }

    // Timestamp 2: execution reports have been handed to the kernel
    auto t_end = std::chrono::high_resolution_clock::now();
    auto duration_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        t_end - t_start).count();

    telemetry.record(duration_ns);  // Single array write, < 1ns cost
}
```

Query packets (`'O'`, `'M'`, `'D'`) use `continue` and skip the telemetry recording entirely. Visualizer queries do not appear in the latency measurements. Only real trading operations are measured.

## Code: Percentile Calculation at Shutdown

```cpp
// include/telemetry.hpp — dump_percentiles()
std::vector<int64_t> sorted_samples(samples_.begin(),
                                     samples_.begin() + count);
std::sort(sorted_samples.begin(), sorted_samples.end());

auto get_p = [&](double p) -> int64_t {
    size_t idx = static_cast<size_t>((p / 100.0) * count);
    if (idx >= count) idx = count - 1;
    return sorted_samples[idx];
};

std::cout << "p50:   " << get_p(50.0)  << " ns\n";
std::cout << "p90:   " << get_p(90.0)  << " ns\n";
std::cout << "p99:   " << get_p(99.0)  << " ns\n";
std::cout << "p99.9: " << get_p(99.9)  << " ns\n";
std::cout << "Max:   " << sorted_samples.back() << " ns\n";
```

## Reading the Output

Run the load test with `python3 tests/client_simulator.py --load` and then send Ctrl+C to the engine. A percentile table will print. The numbers to pay attention to are p50 (the typical order) and the gap between p99 and p99.9 (which reveals whether extreme outliers exist, usually caused by the OS scheduler).
