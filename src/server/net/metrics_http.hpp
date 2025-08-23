// SPDX-License-Identifier: Apache-2.0
// metrics_http.hpp
// Simple Prometheus text-format metrics endpoint (HTTP/1.1) for prototype.
#pragma once
#include <coro/coro.hpp>
#include <coro/io_scheduler.hpp>

#include <cstdint>
#include <memory>

namespace t2d::net {

coro::task<void> run_metrics_endpoint(std::shared_ptr<coro::io_scheduler> scheduler, uint16_t port);

} // namespace t2d::net
