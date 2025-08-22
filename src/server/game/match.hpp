#pragma once
#include <vector>
#include <memory>
#include <chrono>
#include <cstdint>
#include <coro/io_scheduler.hpp>
#include <coro/coro.hpp>
#include "server/matchmaking/session_manager.hpp"
#include "game.pb.h"

namespace t2d::game {

struct TankStateSimple {
    uint32_t entity_id;
    float x{0};
    float y{0};
    float hull_angle{0};
    float turret_angle{0};
    uint32_t hp{100};
    uint32_t ammo{10};
};

struct MatchContext {
    std::string match_id;
    uint32_t tick_rate{30};
    std::vector<std::shared_ptr<t2d::mm::Session>> players;
    std::vector<TankStateSimple> tanks; // parallel by index to players for prototype
    uint64_t server_tick{0};
    uint32_t last_full_snapshot_tick{0};
    // Cache of last sent tank states for delta computation
    std::vector<TankStateSimple> last_sent_tanks;
    struct ProjectileSimple { uint32_t id; float x; float y; float vx; float vy; uint32_t owner; };
    std::vector<ProjectileSimple> projectiles;
    uint32_t next_projectile_id{1};
    // Removed entities since last full snapshot (for delta)
    std::vector<uint32_t> removed_projectiles_since_full;
    std::vector<uint32_t> removed_tanks_since_full; // future (on disconnect / destroy)
};

inline float movement_speed() { return 2.0f; } // units per second (prototype)
inline float turn_speed_deg() { return 90.0f; } // degrees per second
inline float turret_turn_speed_deg() { return 120.0f; }

coro::task<void> run_match(std::shared_ptr<coro::io_scheduler> scheduler, std::shared_ptr<MatchContext> ctx);

} // namespace t2d::game
