// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "game.pb.h"
#include "server/game/physics.hpp"
#include "server/matchmaking/session_manager.hpp"

#include <coro/coro.hpp>
#include <coro/io_scheduler.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

namespace t2d::game {

struct MatchContext
{
    std::string match_id;
    uint32_t tick_rate{30};
    uint32_t initial_player_count{0};
    std::vector<std::shared_ptr<t2d::mm::Session>> players;
    // Physics tanks (authoritative). Index aligned with players.
    std::vector<t2d::phys::TankWithTurret> tanks;
    // Shared physics world (created at match start)
    std::unique_ptr<t2d::phys::World> physics_world;
    uint64_t server_tick{0};
    uint32_t last_full_snapshot_tick{0};
    uint32_t snapshot_interval_ticks{5};
    uint32_t full_snapshot_interval_ticks{30};
    uint32_t bot_fire_interval_ticks{15}; // faster bot fire cadence (reduced for tests)
    float movement_speed{2.0f};
    uint32_t projectile_damage{50}; // increased damage to ensure lethal sequences within test timeout
    float reload_interval_sec{3.0f};
    float projectile_speed{5.0f};
    float projectile_density{0.01f};
    float hull_density{1.0f};
    float turret_density{0.5f};
    bool disable_bot_fire{false}; // when true bots never set fire input

    // Cached last sent snapshot state (angles/positions/ammo/hp) for delta generation.
    struct SentTankCache
    {
        uint32_t entity_id{0};
        float x{0};
        float y{0};
        float hull_angle{0};
        float turret_angle{0};
        uint32_t hp{0};
        uint32_t ammo{0};
        bool alive{false};
    };

    std::vector<SentTankCache> last_sent_tanks;

    struct ProjectileSimple
    {
        uint32_t id;
        float x;
        float y;
        float vx;
        float vy;
        uint32_t owner;
    };

    std::vector<ProjectileSimple> projectiles;
    uint32_t next_projectile_id{1};
    // Removed entities since last full snapshot (for delta)
    std::vector<uint32_t> removed_projectiles_since_full;
    std::vector<uint32_t> removed_tanks_since_full; // future (on disconnect / destroy)
    // Simple per-tank reload timers (seconds until next ammo +1); 0 = ready to accumulate
    std::vector<float> reload_timers;
    uint32_t max_ammo{10};
    bool match_over{false};
    uint32_t winner_entity{0};
    // Aggregated kill feed events for batching per tick (victim, attacker)
    std::vector<std::pair<uint32_t, uint32_t>> kill_feed_events;
};

inline float movement_speed()
{
    return 2.0f;
} // units per second (prototype)

inline float turn_speed_deg()
{
    return 90.0f;
} // degrees per second

inline float turret_turn_speed_deg()
{
    return 120.0f;
}

coro::task<void> run_match(std::shared_ptr<coro::io_scheduler> scheduler, std::shared_ptr<MatchContext> ctx);

} // namespace t2d::game
