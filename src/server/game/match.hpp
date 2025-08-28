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
    // Default projectile density raised significantly to reduce exaggerated ricochet and stabilize penetration.
    float projectile_density{20.0f};
    float fire_cooldown_sec{0.25f};
    float hull_density{1.0f};
    float turret_density{0.5f};
    bool disable_bot_fire{false}; // when true bots never set fire input
    bool disable_bot_ai{false}; // when true bots receive zero input (idle)
    bool test_mode{false}; // toggles test-oriented balancing clamps
    // Map dimensions (authoritative bounds) and static wall bodies created at match start.
    float map_width{300.f};
    float map_height{200.f};

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
        // Pre-step (previous tick integration state) position & velocity captured before physics step
        float prev_x{0.f};
        float prev_y{0.f};
        float prev_vx{0.f};
        float prev_vy{0.f};
        uint32_t owner;
        float initial_speed{0.f};
        float age{0.f}; // seconds since spawn
    };

    float projectile_max_lifetime_sec{5.0f}; // lifespan cap after spawn

    std::vector<ProjectileSimple> projectiles;
    uint32_t next_projectile_id{1};
    // Projectile object pool (freelist indices into projectiles_storage)
    std::vector<ProjectileSimple> projectiles_storage; // stable capacity, entries reused
    std::vector<uint32_t> projectile_free_indices; // indices in storage available for reuse
    uint32_t projectile_pool_hwm{0}; // high-water mark (for future heuristics)

    // Crates (movable obstacles) represented only by physics bodies; snapshot not yet serialized (visual client side
    // placeholder optional)
    struct CrateInfo
    {
        uint32_t id;
        b2BodyId body;
    };

    std::vector<CrateInfo> crates;
    uint32_t next_crate_id{1};

    struct SentCrateCache
    {
        uint32_t id{0};
        float x{0};
        float y{0};
        float angle{0};
        bool alive{false};
    };

    std::vector<SentCrateCache> last_sent_crates; // cached for delta comparison
    std::vector<uint32_t> removed_crates_since_full;

    struct AmmoBoxInfo
    {
        uint32_t id;
        b2BodyId body;
        bool active;
        float x;
        float y;
    };

    std::vector<AmmoBoxInfo> ammo_boxes; // mirrored to snapshot
    uint32_t next_ammo_box_id{1};
    // Removed entities since last full snapshot (for delta)
    std::vector<uint32_t> removed_projectiles_since_full;
    std::vector<uint32_t> removed_tanks_since_full; // future (on disconnect / destroy)
    // Simple per-tank reload timers (seconds until next ammo +1); 0 = ready to accumulate
    std::vector<float> reload_timers;
    uint32_t max_ammo{10};
    bool match_over{false};
    uint32_t winner_entity{0};
    bool match_end_sent{false};
    // Tick when match_over was first set (for post-end grace period). 0 means not yet recorded.
    uint32_t match_over_tick{0};
    // Additional ticks to keep streaming snapshots after MatchEnd so clients can render final state.
    uint32_t post_end_grace_ticks{0}; // set to tick_rate when match ends (== 1s grace)
    // Aggregated kill feed events for batching per tick (victim, attacker)
    std::vector<std::pair<uint32_t, uint32_t>> kill_feed_events;
    // Reusable scratch buffer for snapshot serialization size estimation (SerializeToString target)
    // Grows on demand, never shrinks during match lifetime. Profiling builds record reuse metric.
    std::string snapshot_scratch;
    // When true, tanks with hp==0 remain in snapshots (corpses) until match end.
    bool persist_destroyed_tanks{false};
    // Damage thresholds (copied from match config)
    uint32_t track_break_hits{1};
    uint32_t turret_disable_front_hits{2};
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
