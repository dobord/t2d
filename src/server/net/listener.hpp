#pragma once

#include <memory>
#include <cstdint>
#include <coro/io_scheduler.hpp>
#include <coro/coro.hpp>

namespace t2d::net {

// Starts the TCP accept loop on the given port.
coro::task<void> run_listener(std::shared_ptr<coro::io_scheduler> scheduler, uint16_t port);

} // namespace t2d::net
