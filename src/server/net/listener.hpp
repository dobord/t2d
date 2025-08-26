// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <coro/coro.hpp>
#include <coro/io_scheduler.hpp>

#include <cstdint>
#include <memory>

namespace t2d::net {

// Starts the TCP accept loop on the given port.
// poll/read timeout inside each connection loop is derived from tick_rate to
// keep outbound flush latency bounded relative to simulation ticks.
coro::task<void> run_listener(std::shared_ptr<coro::io_scheduler> scheduler, uint16_t port, uint32_t tick_rate);

} // namespace t2d::net
