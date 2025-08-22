#pragma once

#include <coro/coro.hpp>
#include <coro/io_scheduler.hpp>

#include <cstdint>
#include <memory>

namespace t2d::mm {

struct MatchConfig
{
    uint32_t max_players;
    uint32_t fill_timeout_seconds;
    uint32_t tick_rate;
    uint32_t poll_interval_ms{200};
    uint32_t snapshot_interval_ticks{5};
    uint32_t full_snapshot_interval_ticks{30};
};

coro::task<void> run_matchmaker(std::shared_ptr<coro::io_scheduler> scheduler, MatchConfig cfg);

} // namespace t2d::mm
