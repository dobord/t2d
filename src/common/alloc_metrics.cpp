// SPDX-License-Identifier: Apache-2.0
// Global allocation instrumentation for profiling build.
// Counts allocations (not size) to approximate allocations_per_tick.
#include "common/metrics.hpp"

#include <atomic>
#include <cstdlib>
#include <new>

#if defined(T2D_ENABLE_PROFILING)

void *operator new(std::size_t sz)
{
    void *p = std::malloc(sz);
    if (!p)
        throw std::bad_alloc();
    t2d::metrics::runtime().allocations_total.fetch_add(1, std::memory_order_relaxed);
    return p;
}

void operator delete(void *p) noexcept
{
    std::free(p);
}

void *operator new[](std::size_t sz)
{
    void *p = std::malloc(sz);
    if (!p)
        throw std::bad_alloc();
    t2d::metrics::runtime().allocations_total.fetch_add(1, std::memory_order_relaxed);
    return p;
}

void operator delete[](void *p) noexcept
{
    std::free(p);
}

#endif // T2D_ENABLE_PROFILING
