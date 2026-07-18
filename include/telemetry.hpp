#pragma once

#include <cstdint>
#include <array>
#include <vector>
#include <algorithm>
#include <iostream>
#include <iomanip>

// TelemetryBuffer: pre-allocated ring buffer for latency samples.
// 
// Capturing latency must not perturb the critical path. This class
// allocates exactly once and does nothing but one array write and an
// index increment on record().
//
// Analysis (sorting for percentiles) only happens when the engine
// is shutting down.
class TelemetryBuffer {
public:
    static constexpr size_t CAPACITY = 1'000'000;

    TelemetryBuffer() : index_(0), wrapped_(false) {}

    // Record a latency measurement in nanoseconds.
    // O(1) operation. No branching on the happy path (assuming modulo is cheap,
    // though for ultimate performance index mask with power-of-2 is better,
    // but a branchless modulo or simple branch is fine here).
    inline void record(int64_t ns) {
        samples_[index_] = ns;
        index_++;
        if (index_ >= CAPACITY) {
            index_ = 0;
            wrapped_ = true;
        }
    }

    // Dump p50, p90, p99, p99.9, and p99.99 percentiles to stdout.
    void dump_percentiles() const {
        size_t count = wrapped_ ? CAPACITY : index_;
        if (count == 0) {
            std::cout << "No telemetry data collected.\n";
            return;
        }

        std::vector<int64_t> sorted_samples(count);
        std::copy(samples_.begin(), samples_.begin() + count, sorted_samples.begin());
        std::sort(sorted_samples.begin(), sorted_samples.end());

        auto get_p = [&](double p) -> int64_t {
            size_t idx = static_cast<size_t>((p / 100.0) * count);
            if (idx >= count) idx = count - 1;
            return sorted_samples[idx];
        };

        std::cout << "\n=== LATENCY PERCENTILES (ns) ===\n";
        std::cout << "Samples: " << count << "\n";
        std::cout << "Min:     " << sorted_samples.front() << " ns\n";
        std::cout << "p50:     " << get_p(50.0) << " ns\n";
        std::cout << "p90:     " << get_p(90.0) << " ns\n";
        std::cout << "p99:     " << get_p(99.0) << " ns\n";
        std::cout << "p99.9:   " << get_p(99.9) << " ns\n";
        std::cout << "p99.99:  " << get_p(99.99) << " ns\n";
        std::cout << "Max:     " << sorted_samples.back() << " ns\n";
        std::cout << "================================\n";
    }

private:
    std::array<int64_t, CAPACITY> samples_;
    size_t index_;
    bool wrapped_;
};
