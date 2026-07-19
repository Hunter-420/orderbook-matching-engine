# Phase 4: Hardware Isolation and Latency Measurement

The final phase aims to achieve **deterministic latency** (preventing unpredictable latency spikes) and measuring our performance in nanoseconds.

## Design

Modern operating systems aggressively balance workloads by migrating threads between CPU cores. When a thread moves to a new core, the L1 and L2 caches are completely cold. The CPU must pull memory from main RAM (taking ~100ns per read instead of ~1ns), causing massive latency spikes (jitter).

To solve this, we use CPU Affinity Pinning.

We also need to measure latency, but the act of measuring it (printing it, or doing complex math) often slows down the engine! We solve this using a pre-allocated `TelemetryBuffer`.

## Code Snippets

### CPU Affinity Pinning
We pin our single thread to one specific CPU core using `pthread_setaffinity_np`. Once pinned, the Linux scheduler will not migrate our process, keeping our `MemoryPool` permanently hot in that core's L1/L2 cache.

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
Our telemetry buffer is just a flat array allocated once. Recording a timestamp takes `O(1)` time with no branching (or minimal branching).

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
Because sorting the array to find the p99/p99.9 percentiles takes significant CPU time, we NEVER do this while the engine is running. We trap `SIGINT` (Ctrl+C) to stop the engine cleanly, and only then do we crunch the numbers.

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
