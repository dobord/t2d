// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "common/logger.hpp"

#include <cstdint>

// Lightweight per-callsite rate-limited logging macro.
// Emits a log line every Nth invocation at the given level without extra branching or heap allocs.
// Usage: T2D_LOG_EVERY_N(debug, 60, "message {}", value);
// Rationale: High-frequency trace/debug in the tick loop (drive/projectile events) caused unnecessary
// formatting & I/O overhead even when log level filters later discard them. This macro reduces that cost.
#define T2D_LOG_EVERY_N(level, N, ...) \
    do { \
        static uint64_t _t2d_log_counter_##__LINE__ = 0; \
        if ((++_t2d_log_counter_##__LINE__ % (N)) == 0) { \
            t2d::log::level(__VA_ARGS__); \
        } \
    } while (0)
