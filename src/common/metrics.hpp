// SPDX-License-Identifier: Apache-2.0
// metrics.hpp
// Prototype metrics counters (atomics, no dynamic allocation). Profiling-only helpers wrapped by T2D_PROFILING_ENABLED.
#pragma once
#include <atomic>
#include <cstdint>

// Backward compatibility: allow legacy CMake option T2D_ENABLE_PROFILING to imply macro.
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
    // Power-of-two buckets for tick & wait durations (base 250k ns) -> up to ~128ms.
    static constexpr int TICK_BUCKETS = 10;
    std::atomic<uint64_t> tick_hist[TICK_BUCKETS]{}; // bucket 0:<250k,1:<500k,...
    std::atomic<uint64_t> wait_duration_ns_accum{0};
    std::atomic<uint64_t> wait_samples{0};
    std::atomic<uint64_t> wait_hist[TICK_BUCKETS]{};
    // Realtime queue / game object gauges
    std::atomic<uint64_t> queue_depth{0};
    std::atomic<uint64_t> active_matches{0};
    std::atomic<uint64_t> bots_in_match{0};
    std::atomic<uint64_t> connected_players{0};
    std::atomic<uint64_t> projectiles_active{0};
    std::atomic<uint64_t> auth_failures{0};
    // Process resource accumulators
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
    static constexpr int ALLOC_BUCKETS = 12; // 1,2,4,...,1024,overflow
    std::atomic<uint64_t> allocations_per_tick_hist[ALLOC_BUCKETS]{};
    // Deallocation metrics
    std::atomic<uint64_t> deallocations_total{0};
    std::atomic<uint64_t> deallocations_per_tick_accum{0};
    std::atomic<uint64_t> deallocations_per_tick_samples{0};
    std::atomic<uint64_t> deallocations_ticks_with_free{0};
    // Snapshot scratch buffer reuse
    std::atomic<uint64_t> snapshot_scratch_requests{0};
    std::atomic<uint64_t> snapshot_scratch_reused{0};
    // Projectile pool
    std::atomic<uint64_t> projectile_pool_requests{0};
    std::atomic<uint64_t> projectile_pool_hits{0};
    std::atomic<uint64_t> projectile_pool_misses{0};
    // Logging (profiling): lines per tick
    std::atomic<uint64_t> log_lines_total{0};
    std::atomic<uint64_t> log_lines_per_tick_accum{0};
    std::atomic<uint64_t> log_lines_per_tick_samples{0};
};

inline RuntimeCounters &runtime()
{
    static RuntimeCounters inst;
    return inst;
}

// --- Tick duration histogram ---
inline void add_tick_duration(uint64_t ns)
{
    auto &rt = runtime();
    rt.tick_duration_ns_accum.fetch_add(ns, std::memory_order_relaxed);
    rt.tick_samples.fetch_add(1, std::memory_order_relaxed);
    constexpr uint64_t base = 250000; // 0.25ms
    for (int i = 0; i < RuntimeCounters::TICK_BUCKETS; ++i) {
        uint64_t bound = base << i;
        if (ns < bound) {
            rt.tick_hist[i].fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    rt.tick_hist[RuntimeCounters::TICK_BUCKETS - 1].fetch_add(1, std::memory_order_relaxed);
}

inline uint64_t approx_tick_p99()
{
    auto &rt = runtime();
    uint64_t total = rt.tick_samples.load(std::memory_order_relaxed);
    if (total == 0)
        return 0;
    uint64_t target = (total * 99 + 99 - 1) / 100; // ceil(total*0.99)
    constexpr uint64_t base = 250000;
    uint64_t cumulative = 0;
    for (int i = 0; i < RuntimeCounters::TICK_BUCKETS; ++i) {
        cumulative += rt.tick_hist[i].load(std::memory_order_relaxed);
        if (cumulative >= target) {
            return (base << i);
        }
    }
    return (base << (RuntimeCounters::TICK_BUCKETS - 1));
}

// --- Off-CPU wait histogram ---
inline void add_wait_duration(uint64_t ns)
{
    auto &rt = runtime();
    rt.wait_duration_ns_accum.fetch_add(ns, std::memory_order_relaxed);
    rt.wait_samples.fetch_add(1, std::memory_order_relaxed);
    constexpr uint64_t base = 250000;
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

// ---- Profiling-only helpers ----
#if T2D_PROFILING_ENABLED
inline void add_allocations_tick(uint64_t count)
{
    auto &rt = runtime();
    for (int i = 0; i < RuntimeCounters::ALLOC_BUCKETS - 1; ++i) {
        uint64_t bound = (uint64_t)1 << i;
        if (count < bound) {
            rt.allocations_per_tick_hist[i].fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    rt.allocations_per_tick_hist[RuntimeCounters::ALLOC_BUCKETS - 1].fetch_add(1, std::memory_order_relaxed);
}

inline void add_log_lines_tick(uint64_t count)
{
    auto &rt = runtime();
    rt.log_lines_per_tick_accum.fetch_add(count, std::memory_order_relaxed);
    rt.log_lines_per_tick_samples.fetch_add(1, std::memory_order_relaxed);
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
                return (uint64_t)1 << (RuntimeCounters::ALLOC_BUCKETS - 1);
            }
            return (uint64_t)1 << i; // approximate upper bound
        }
    }
    return (uint64_t)1 << (RuntimeCounters::ALLOC_BUCKETS - 1);
}

inline void add_snapshot_scratch_usage(bool reused)
{
    auto &rt = runtime();
    rt.snapshot_scratch_requests.fetch_add(1, std::memory_order_relaxed);
    if (reused)
        rt.snapshot_scratch_reused.fetch_add(1, std::memory_order_relaxed);
}

inline double snapshot_scratch_reuse_pct()
{
    auto &rt = runtime();
    auto req = rt.snapshot_scratch_requests.load(std::memory_order_relaxed);
    if (req == 0)
        return 0.0;
    auto reused = rt.snapshot_scratch_reused.load(std::memory_order_relaxed);
    return 100.0 * (double)reused / (double)req;
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

inline void add_log_lines_tick(uint64_t) {}

inline uint64_t approx_allocations_per_tick_p95()
{
    return 0;
}

inline void add_snapshot_scratch_usage(bool) {}

inline double snapshot_scratch_reuse_pct()
{
    return 0.0;
}

inline void add_projectile_pool_request(bool, bool) {}

inline double projectile_pool_hit_pct()
{
    return 0.0;
}

inline uint64_t projectile_pool_misses()
{
    return 0;
}
#endif

// Snapshot counters accessors
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
