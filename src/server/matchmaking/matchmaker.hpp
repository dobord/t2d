// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <coro/coro.hpp>
#include <coro/io_scheduler.hpp>

#include <cstdint>
#include <memory>

namespace t2d::mm {

struct MatchConfig
{
    // Defaults aligned with test-oriented profile (server_test.yaml)
    uint32_t max_players{4};
    uint32_t fill_timeout_seconds{2};
    uint32_t tick_rate{30};
    uint32_t poll_interval_ms{100};
    uint32_t snapshot_interval_ticks{5};
    uint32_t full_snapshot_interval_ticks{30};
    uint32_t bot_fire_interval_ticks{5};
    float movement_speed{2.5f};
    uint32_t projectile_damage{50};
    float reload_interval_sec{1.5f};
    float projectile_speed{10.0f};
    // Increased default to align with gameplay tuning (higher mass-like behavior)
    float projectile_density{20.0f};
    float projectile_max_lifetime_sec{5.0f};
    float fire_cooldown_sec{0.25f};
    float hull_density{5.0f};
    float turret_density{2.5f};
    bool disable_bot_fire{false};
    bool disable_bot_ai{false};
    bool test_mode{true};
    float map_width{80.f};
    float map_height{80.f};
    // Test hook: when true, spawn players in a horizontal line centered at origin (even spacing) instead of random.
    bool force_line_spawn{false};
    bool persist_destroyed_tanks{false};
    // Damage system thresholds
    uint32_t track_break_hits{1}; // hits to a side before that track breaks
    uint32_t turret_disable_front_hits{2}; // frontal hits to disable turret motor
    // Optional fixed seed override; when >0 use this instead of random_seed()
    uint32_t fixed_seed{0};
};

coro::task<void> run_matchmaker(std::shared_ptr<coro::io_scheduler> scheduler, MatchConfig cfg);

} // namespace t2d::mm
