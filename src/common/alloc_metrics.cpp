// SPDX-License-Identifier: Apache-2.0
// Global allocation instrumentation for profiling build.
// Counts allocations (not size) to approximate allocations_per_tick.
#include "common/metrics.hpp"

#include <atomic>
#include <cstdlib>
#include <new>

#if defined(T2D_ENABLE_PROFILING)

static inline void t2d_alloc_account(std::size_t sz)
{
    auto &rt = t2d::metrics::runtime();
    rt.allocations_total.fetch_add(1, std::memory_order_relaxed);
    rt.allocations_bytes_total.fetch_add(sz, std::memory_order_relaxed);
}

static inline void t2d_free_account()
{
    auto &rt = t2d::metrics::runtime();
    rt.deallocations_total.fetch_add(1, std::memory_order_relaxed);
}

void *operator new(std::size_t sz)
{
    void *p = std::malloc(sz);
    if (!p)
        throw std::bad_alloc();
    t2d_alloc_account(sz);
    return p;
}

void *operator new[](std::size_t sz)
{
    void *p = std::malloc(sz);
    if (!p)
        throw std::bad_alloc();
    t2d_alloc_account(sz);
    return p;
}

void *operator new(std::size_t sz, const std::nothrow_t &) noexcept
{
    void *p = std::malloc(sz);
    if (p)
        t2d_alloc_account(sz);
    return p;
}

void *operator new[](std::size_t sz, const std::nothrow_t &) noexcept
{
    void *p = std::malloc(sz);
    if (p)
        t2d_alloc_account(sz);
    return p;
}

void *operator new(std::size_t sz, std::align_val_t al)
{
    void *p = ::operator new(sz, al, std::nothrow);
    if (!p)
        throw std::bad_alloc();
    t2d_alloc_account(sz);
    return p;
}

void *operator new[](std::size_t sz, std::align_val_t al)
{
    void *p = ::operator new[](sz, al, std::nothrow);
    if (!p)
        throw std::bad_alloc();
    t2d_alloc_account(sz);
    return p;
}

void *operator new(std::size_t sz, std::align_val_t al, const std::nothrow_t &) noexcept
{
    void *p = std::aligned_alloc((size_t)al, ((sz + (size_t)al - 1) / (size_t)al) * (size_t)al);
    if (p)
        t2d_alloc_account(sz);
    return p;
}

void *operator new[](std::size_t sz, std::align_val_t al, const std::nothrow_t &) noexcept
{
    void *p = std::aligned_alloc((size_t)al, ((sz + (size_t)al - 1) / (size_t)al) * (size_t)al);
    if (p)
        t2d_alloc_account(sz);
    return p;
}

void operator delete(void *p) noexcept
{
    if (p) {
        t2d_free_account();
        std::free(p);
    }
}

void operator delete[](void *p) noexcept
{
    if (p) {
        t2d_free_account();
        std::free(p);
    }
}

void operator delete(void *p, const std::nothrow_t &) noexcept
{
    if (p) {
        t2d_free_account();
        std::free(p);
    }
}

void operator delete[](void *p, const std::nothrow_t &) noexcept
{
    if (p) {
        t2d_free_account();
        std::free(p);
    }
}

void operator delete(void *p, std::size_t) noexcept
{
    if (p) {
        t2d_free_account();
        std::free(p);
    }
}

void operator delete[](void *p, std::size_t) noexcept
{
    if (p) {
        t2d_free_account();
        std::free(p);
    }
}

void operator delete(void *p, std::align_val_t) noexcept
{
    if (p) {
        t2d_free_account();
        std::free(p);
    }
}

void operator delete[](void *p, std::align_val_t) noexcept
{
    if (p) {
        t2d_free_account();
        std::free(p);
    }
}

void operator delete(void *p, std::align_val_t, std::size_t) noexcept
{
    if (p) {
        t2d_free_account();
        std::free(p);
    }
}

void operator delete[](void *p, std::align_val_t, std::size_t) noexcept
{
    if (p) {
        t2d_free_account();
        std::free(p);
    }
}

void operator delete(void *p, std::align_val_t, const std::nothrow_t &) noexcept
{
    if (p) {
        t2d_free_account();
        std::free(p);
    }
}

void operator delete[](void *p, std::align_val_t, const std::nothrow_t &) noexcept
{
    if (p) {
        t2d_free_account();
        std::free(p);
    }
}

#endif // T2D_ENABLE_PROFILING
