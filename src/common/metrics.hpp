// SPDX-License-Identifier: Apache-2.0
// metrics.hpp
// Simple atomic counters for prototype metrics collection (snapshot sizes, counts).
#pragma once
#include <atomic>
#include <cstdint>

namespace t2d::metrics {

struct SnapshotCounters
{
    std::atomic<uint64_t> full_bytes{0};
    std::atomic<uint64_t> delta_bytes{0};
    std::atomic<uint64_t> full_count{0};
    std::atomic<uint64_t> delta_count{0};
    std::atomic<uint64_t> full_compressed_bytes{0};
    std::atomic<uint64_t> delta_compressed_bytes{0};
};

struct RuntimeCounters
{
    std::atomic<uint64_t> tick_duration_ns_accum{0};
    std::atomic<uint64_t> tick_samples{0};
    // Basic power-of-two bucket histogram (nanoseconds) for tick durations (prototype)
    static constexpr int TICK_BUCKETS = 10; // up to 2^(9) bucket (~512x base)
    std::atomic<uint64_t> tick_hist[TICK_BUCKETS]{}; // bucket 0: <250k ns, 1: <500k, etc (see add_tick_duration)
    // Off-CPU wait (sleep between ticks) histogram & accumulators (same bucket strategy, base 250k ns)
    std::atomic<uint64_t> wait_duration_ns_accum{0};
    std::atomic<uint64_t> wait_samples{0};
    std::atomic<uint64_t> wait_hist[TICK_BUCKETS]{};
    std::atomic<uint64_t> queue_depth{0};
    std::atomic<uint64_t> active_matches{0};
    std::atomic<uint64_t> bots_in_match{0};
    std::atomic<uint64_t> connected_players{0}; // authenticated non-bot sessions currently connected
    std::atomic<uint64_t> projectiles_active{0};
    std::atomic<uint64_t> auth_failures{0};
    // Resource metrics (process-wide) sampled periodically
    std::atomic<uint64_t> user_cpu_ns_accum{0};
    std::atomic<uint64_t> wall_clock_ns_accum{0};
    std::atomic<uint64_t> rss_peak_bytes{0};
    // Allocation metrics (profiling build)
    std::atomic<uint64_t> allocations_total{0};
    std::atomic<uint64_t> allocations_per_tick_accum{0};
    std::atomic<uint64_t> allocations_per_tick_samples{0};
};

inline RuntimeCounters &runtime()
{
    static RuntimeCounters inst;
    return inst;
}

inline void add_tick_duration(uint64_t ns)
{
    runtime().tick_duration_ns_accum.fetch_add(ns, std::memory_order_relaxed);
    runtime().tick_samples.fetch_add(1, std::memory_order_relaxed);
    // Histogram bucket boundaries starting at 250k ns (0.25ms). Geometric progression (x2) up to ~128ms.
    constexpr uint64_t base = 250000; // 0.25ms
    auto &rt = runtime();
    for (int i = 0; i < RuntimeCounters::TICK_BUCKETS; ++i) {
        uint64_t bound = base << i; // multiply by 2 each bucket
        if (ns < bound) {
            rt.tick_hist[i].fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    // Overflow bucket: last index
    rt.tick_hist[RuntimeCounters::TICK_BUCKETS - 1].fetch_add(1, std::memory_order_relaxed);
}

inline uint64_t approx_tick_p99()
{
    auto &rt = runtime();
    uint64_t total = rt.tick_samples.load(std::memory_order_relaxed);
    if (total == 0)
        return 0;
    // Rank (ceiling) for 99th percentile
    uint64_t target = (total * 99 + 99 - 1) / 100; // ceil(total*0.99)
    constexpr uint64_t base = 250000; // same base as add_tick_duration
    uint64_t cumulative = 0;
    for (int i = 0; i < RuntimeCounters::TICK_BUCKETS; ++i) {
        cumulative += rt.tick_hist[i].load(std::memory_order_relaxed);
        if (cumulative >= target) {
            // Return upper bound of this bucket as approximate p99
            return (base << i);
        }
    }
    return (base << (RuntimeCounters::TICK_BUCKETS - 1));
}

inline void add_wait_duration(uint64_t ns)
{
    runtime().wait_duration_ns_accum.fetch_add(ns, std::memory_order_relaxed);
    runtime().wait_samples.fetch_add(1, std::memory_order_relaxed);
    constexpr uint64_t base = 250000; // 0.25ms buckets (mirror tick duration)
    auto &rt = runtime();
    for (int i = 0; i < RuntimeCounters::TICK_BUCKETS; ++i) {
        uint64_t bound = base << i;
        if (ns < bound) {
            rt.wait_hist[i].fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    rt.wait_hist[RuntimeCounters::TICK_BUCKETS - 1].fetch_add(1, std::memory_order_relaxed);
}

inline uint64_t approx_wait_p99()
{
    auto &rt = runtime();
    uint64_t total = rt.wait_samples.load(std::memory_order_relaxed);
    if (total == 0)
        return 0;
    uint64_t target = (total * 99 + 99 - 1) / 100;
    constexpr uint64_t base = 250000;
    uint64_t cumulative = 0;
    for (int i = 0; i < RuntimeCounters::TICK_BUCKETS; ++i) {
        cumulative += rt.wait_hist[i].load(std::memory_order_relaxed);
        if (cumulative >= target) {
            return (base << i);
        }
    }
    return (base << (RuntimeCounters::TICK_BUCKETS - 1));
}

inline SnapshotCounters &snapshot()
{
    static SnapshotCounters inst;
    return inst;
}

inline void add_full(uint64_t bytes)
{
    snapshot().full_bytes.fetch_add(bytes, std::memory_order_relaxed);
    snapshot().full_count.fetch_add(1, std::memory_order_relaxed);
}

inline void add_delta(uint64_t bytes)
{
    snapshot().delta_bytes.fetch_add(bytes, std::memory_order_relaxed);
    snapshot().delta_count.fetch_add(1, std::memory_order_relaxed);
}

inline void add_full_compressed(uint64_t bytes)
{
    snapshot().full_compressed_bytes.fetch_add(bytes, std::memory_order_relaxed);
}

inline void add_delta_compressed(uint64_t bytes)
{
    snapshot().delta_compressed_bytes.fetch_add(bytes, std::memory_order_relaxed);
}

} // namespace t2d::metrics
