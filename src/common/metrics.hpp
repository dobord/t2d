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
};

struct RuntimeCounters
{
    std::atomic<uint64_t> tick_duration_ns_accum{0};
    std::atomic<uint64_t> tick_samples{0};
    std::atomic<uint64_t> queue_depth{0};
    std::atomic<uint64_t> active_matches{0};
    std::atomic<uint64_t> bots_in_match{0};
    std::atomic<uint64_t> projectiles_active{0};
    std::atomic<uint64_t> auth_failures{0};
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

} // namespace t2d::metrics
