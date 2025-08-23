// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <coro/coro.hpp>
#include <coro/io_scheduler.hpp>

#include <cstdint>
#include <memory>

namespace t2d::net {

// Starts the TCP accept loop on the given port.
coro::task<void> run_listener(std::shared_ptr<coro::io_scheduler> scheduler, uint16_t port);

} // namespace t2d::net
