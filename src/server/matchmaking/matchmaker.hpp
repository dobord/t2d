#pragma once

#include <cstdint>
#include <memory>
#include <coro/io_scheduler.hpp>
#include <coro/coro.hpp>

namespace t2d::mm {

struct MatchConfig {
	uint32_t max_players;
	uint32_t fill_timeout_seconds;
	uint32_t tick_rate;
	uint32_t poll_interval_ms{200};
};
coro::task<void> run_matchmaker(std::shared_ptr<coro::io_scheduler> scheduler, MatchConfig cfg);

} // namespace t2d::mm
