# Phase 4: Hardware Isolation and Latency Measurement

The final phase aims to achieve **deterministic latency** (preventing unpredictable latency spikes) and measuring our performance in nanoseconds.

## Logic Explanation: CPU Affinity and Non-blocking Telemetry
Modern operating systems aggressively balance workloads by migrating threads between CPU cores. When a thread moves to a new core, the L1 and L2 caches on that core are completely cold. The CPU must pull memory from main RAM (taking ~100ns per read instead of ~1ns), causing massive latency spikes (jitter).

To solve this, we use **CPU Affinity Pinning** to lock our thread to exactly one core, permanently.

We also need to measure how fast we are. But if we try to `std::cout` our latency for every order, printing to the screen takes thousands of nanoseconds, which ruins the very thing we are trying to measure! 
Instead, we use a `TelemetryBuffer`—a massive, pre-allocated array of 1,000,000 slots. When an order arrives, we record the start time. When the execution report goes out, we record the end time. We subtract the two, and write the difference into the array at the current index, then increment the index. Writing to a pre-allocated array takes `< 1ns`. 

We only sort the array and calculate percentiles (p50, p99) when the user gracefully shuts down the engine via `Ctrl+C`.

### Example
1. The engine starts and locks itself to **Core 1**.
2. **Order 1** arrives. Start time: `10,000 ns`. 
3. The engine matches it and sends a fill. End time: `12,500 ns`.
4. Latency is `2,500 ns`. The engine writes `2,500` to `TelemetryBuffer[0]` and increments the index to 1.
5. **Order 2** arrives. Engine writes `3,100` to `TelemetryBuffer[1]`.
6. User hits `Ctrl+C`. The engine stops accepting orders, sorts `[2500, 3100]`, and prints: `p50: 2500 ns, p99: 3100 ns`.

## Code Snippets

### CPU Affinity Pinning
We pin our single thread to one specific CPU core using `pthread_setaffinity_np`. 

```cpp
// src/main.cpp
void pin_to_core(int core_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);

    pthread_t current_thread = pthread_self();
    pthread_setaffinity_np(current_thread, sizeof(cpu_set_t), &cpuset);
}

// In main():
pin_to_core(1); // Pin engine exclusively to Core 1
```

### Telemetry Buffer
Recording a timestamp takes `O(1)` time with no branching (or minimal branching).

```cpp
// include/telemetry.hpp
class TelemetryBuffer {
public:
    static constexpr size_t CAPACITY = 1'000'000;
    
    inline void record(int64_t ns) {
        samples_[index_] = ns;
        index_++;
        if (index_ >= CAPACITY) {
            index_ = 0;
            wrapped_ = true;
        }
    }
private:
    std::array<int64_t, CAPACITY> samples_;
    size_t index_;
    bool wrapped_;
};
```

### Nanosecond Timestamping
We measure latency from the instant bytes hit user-space until the instant the execution report is pushed back to the kernel.

```cpp
// src/main.cpp (inside the epoll loop)
if (bytes == sizeof(OrderRequest)) {
    // 1. Capture start time
    auto t_start = std::chrono::high_resolution_clock::now();

    // 2. Process Order
    OrderRequest req;
    std::memcpy(&req, buf, sizeof(OrderRequest));
    engine.new_order(req.order_id, req.side, req.price, req.quantity);
    
    // 3. Send Execution Reports ...
    
    // 4. Capture end time & record
    auto t_end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(t_end - t_start).count();
    telemetry.record(duration); // O(1) array write, no printing!
}
```

### Percentile Analysis on Shutdown
We trap `SIGINT` (Ctrl+C) to stop the engine cleanly, and only then do we crunch the numbers.

```cpp
// include/telemetry.hpp (snippet from dump_percentiles)
std::sort(sorted_samples.begin(), sorted_samples.end());

auto get_p = [&](double p) -> int64_t {
    size_t idx = static_cast<size_t>((p / 100.0) * count);
    if (idx >= count) idx = count - 1;
    return sorted_samples[idx];
};

std::cout << "p50:     " << get_p(50.0) << " ns\n";
std::cout << "p99.9:   " << get_p(99.9) << " ns\n";
```
