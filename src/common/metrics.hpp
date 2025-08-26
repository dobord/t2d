// SPDX-License-Identifier: Apache-2.0
// metrics.hpp
// Simple atomic counters for prototype metrics collection (snapshot sizes, counts).
#pragma once
#include <atomic>
#include <cstdint>

// Backward compatibility: treat build option T2D_ENABLE_PROFILING (CMake variable) as runtime macro if present via
// T2D_PROFILING_ENABLED; unify on T2D_PROFILING_ENABLED in code (0/1 value defined in CMake interface target).
#if defined(T2D_ENABLE_PROFILING) && !defined(T2D_PROFILING_ENABLED)
#    define T2D_PROFILING_ENABLED 1
#endif

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
    std::atomic<uint64_t> allocations_bytes_total{0};
    std::atomic<uint64_t> allocations_per_tick_accum{0};
    std::atomic<uint64_t> allocations_per_tick_samples{0};
    std::atomic<uint64_t> allocations_bytes_per_tick_accum{0};
    std::atomic<uint64_t> allocations_bytes_per_tick_samples{0};
    std::atomic<uint64_t> allocations_ticks_with_alloc{0};
    // Allocation count per tick histogram (power-of-two buckets) for p95 estimation
    static constexpr int ALLOC_BUCKETS = 12; // up to 2^(11) (~2048) before overflow bucket
    std::atomic<uint64_t> allocations_per_tick_hist[ALLOC_BUCKETS]{}; // bucket 0: <1, 1:<2, ... 10:<1024, 11: overflow
    // Deallocation metrics
    std::atomic<uint64_t> deallocations_total{0};
    std::atomic<uint64_t> deallocations_per_tick_accum{0};
    std::atomic<uint64_t> deallocations_per_tick_samples{0};
    std::atomic<uint64_t> deallocations_ticks_with_free{0};
    // Snapshot scratch buffer reuse metrics (profiling build)
    std::atomic<uint64_t> snapshot_scratch_requests{0};
    std::atomic<uint64_t> snapshot_scratch_reused{0};
    // Projectile pool metrics
    std::atomic<uint64_t> projectile_pool_requests{0};
    std::atomic<uint64_t> projectile_pool_hits{0};
    std::atomic<uint64_t> projectile_pool_misses{0}; // growth events
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

#if T2D_PROFILING_ENABLED
inline void add_allocations_tick(uint64_t count)
{
    // Power-of-two bucket histogram (similar pattern to tick durations) base=1
    auto &rt = runtime();
    for (int i = 0; i < RuntimeCounters::ALLOC_BUCKETS - 1; ++i) {
        uint64_t bound = (uint64_t)1 << i; // 1,2,4,... 1024
        if (count < bound) {
            rt.allocations_per_tick_hist[i].fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    rt.allocations_per_tick_hist[RuntimeCounters::ALLOC_BUCKETS - 1].fetch_add(1, std::memory_order_relaxed);
}

inline uint64_t approx_allocations_per_tick_p95()
{
    auto &rt = runtime();
    uint64_t total = rt.allocations_per_tick_samples.load(std::memory_order_relaxed);
    if (total == 0)
        return 0;
    uint64_t target = (total * 95 + 95 - 1) / 100; // ceil(total*0.95)
    uint64_t cumulative = 0;
    for (int i = 0; i < RuntimeCounters::ALLOC_BUCKETS; ++i) {
        cumulative += rt.allocations_per_tick_hist[i].load(std::memory_order_relaxed);
        if (cumulative >= target) {
            if (i == RuntimeCounters::ALLOC_BUCKETS - 1) {
                // Overflow bucket; approximate as upper bound *2
                return (uint64_t)1 << (RuntimeCounters::ALLOC_BUCKETS - 1);
            }
            return (uint64_t)1 << i; // upper bound of bucket (exclusive) ~ approximate p95
        }
    }
    return (uint64_t)1 << (RuntimeCounters::ALLOC_BUCKETS - 1);
}

inline void add_snapshot_scratch_usage(bool reused)
{
    auto &rt = runtime();
    rt.snapshot_scratch_requests.fetch_add(1, std::memory_order_relaxed);
    if (reused) {
        rt.snapshot_scratch_reused.fetch_add(1, std::memory_order_relaxed);
    }
}

inline double snapshot_scratch_reuse_pct()
{
    auto &rt = runtime();
    auto req = rt.snapshot_scratch_requests.load(std::memory_order_relaxed);
    if (req == 0)
        return 0.0;
    auto reused = rt.snapshot_scratch_reused.load(std::memory_order_relaxed);
    return 100.0 * static_cast<double>(reused) / static_cast<double>(req);
}

inline void add_projectile_pool_request(bool hit, bool miss)
{
    auto &rt = runtime();
    rt.projectile_pool_requests.fetch_add(1, std::memory_order_relaxed);
    if (hit)
        rt.projectile_pool_hits.fetch_add(1, std::memory_order_relaxed);
    if (miss)
        rt.projectile_pool_misses.fetch_add(1, std::memory_order_relaxed);
}

inline double projectile_pool_hit_pct()
{
    auto &rt = runtime();
    auto req = rt.projectile_pool_requests.load(std::memory_order_relaxed);
    if (req == 0)
        return 0.0;
    auto hits = rt.projectile_pool_hits.load(std::memory_order_relaxed);
    return 100.0 * (double)hits / (double)req;
}

inline uint64_t projectile_pool_misses()
{
    return runtime().projectile_pool_misses.load(std::memory_order_relaxed);
}
#else
inline void add_allocations_tick(uint64_t) {}

inline uint64_t approx_allocations_per_tick_p95()
{
    return 0;
}

inline void add_snapshot_scratch_usage(bool) {}

inline double snapshot_scratch_reuse_pct()
{
    return 0.0;
}
#endif

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
