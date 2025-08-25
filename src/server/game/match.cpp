// SPDX-License-Identifier: Apache-2.0
#include "server/game/match.hpp"

#include "common/logger.hpp"
#include "common/metrics.hpp"
#include "server/game/physics.hpp"
#include "server/game/snapshot_compress.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

namespace {

using ProjectileMap = std::unordered_map<uint32_t, b2BodyId>;

static void process_contacts(
    t2d::phys::World &phys_world, ProjectileMap &projectile_bodies, t2d::game::MatchContext &ctx)
{
    const uint32_t damage_amount = ctx.projectile_damage;
    auto events = b2World_GetContactEvents(phys_world.id);
    if (events.beginCount <= 0)
        return;
    std::vector<uint32_t> to_destroy_projectiles;
    for (int i = 0; i < events.beginCount; ++i) {
        const b2ContactBeginTouchEvent &ev = events.beginEvents[i];
        b2BodyId a = b2Shape_GetBody(ev.shapeIdA);
        b2BodyId b = b2Shape_GetBody(ev.shapeIdB);
        uint32_t proj_id = 0;
        bool a_is_proj = false;
        uint32_t tank_index = UINT32_MAX;
        for (auto &kv : projectile_bodies) {
            if (kv.second.index1 == a.index1) {
                proj_id = kv.first;
                a_is_proj = true;
                break;
            }
            if (kv.second.index1 == b.index1) {
                proj_id = kv.first;
                a_is_proj = false;
                break;
            }
        }
        if (proj_id == 0)
            continue;
        for (size_t ti = 0; ti < phys_world.tank_bodies.size(); ++ti) {
            auto id = phys_world.tank_bodies[ti];
            if ((a_is_proj ? b.index1 : a.index1) == id.index1) {
                tank_index = ti;
                break;
            }
        }
        if (tank_index == UINT32_MAX || tank_index >= ctx.tanks.size())
            continue;
        auto &tank = ctx.tanks[tank_index];
        if (tank.hp == 0)
            continue;
        auto pit =
            std::find_if(ctx.projectiles.begin(), ctx.projectiles.end(), [&](auto &p) { return p.id == proj_id; });
        if (pit == ctx.projectiles.end())
            continue;
        auto &proj = *pit;
        if (tank.entity_id == proj.owner)
            continue;
        if (tank.hp > 0) {
            uint16_t before = tank.hp;
            if (tank.hp <= damage_amount)
                tank.hp = 0;
            else
                tank.hp -= damage_amount;
            t2d::ServerMessage evmsg;
            auto *d = evmsg.mutable_damage();
            d->set_victim_id(tank.entity_id);
            d->set_attacker_id(proj.owner);
            d->set_amount(damage_amount);
            d->set_remaining_hp(tank.hp);
            for (auto &pl : ctx.players)
                t2d::mm::instance().push_message(pl, evmsg);
            if (before > 0 && tank.hp == 0) {
                ctx.removed_tanks_since_full.push_back(tank.entity_id);
                ctx.kill_feed_events.emplace_back(tank.entity_id, proj.owner);
                t2d::ServerMessage tdmsg;
                auto *td = tdmsg.mutable_destroyed();
                td->set_victim_id(tank.entity_id);
                td->set_attacker_id(proj.owner);
                for (auto &pl : ctx.players)
                    t2d::mm::instance().push_message(pl, tdmsg);
            }
        }
        auto body_it = projectile_bodies.find(proj_id);
        if (body_it != projectile_bodies.end()) {
            if (b2Body_IsValid(body_it->second)) {
                to_destroy_projectiles.push_back(proj_id);
            } else {
                projectile_bodies.erase(body_it);
            }
        }
        ctx.removed_projectiles_since_full.push_back(proj_id);
        ctx.projectiles.erase(pit);
    }
    for (auto pid : to_destroy_projectiles) {
        auto it = projectile_bodies.find(pid);
        if (it != projectile_bodies.end()) {
            if (b2Body_IsValid(it->second)) {
                t2d::phys::destroy_body(it->second);
            }
            projectile_bodies.erase(it);
        }
    }
}
} // anonymous namespace

namespace t2d::game {

coro::task<void> run_match(std::shared_ptr<coro::io_scheduler> scheduler, std::shared_ptr<MatchContext> ctx)
{
    co_await scheduler->schedule();
    t2d::log::info("[match] start id={} players={}", ctx->match_id, ctx->players.size());
    // Physics world (advanced tank physics with hull+turret)
    // Use existing world if already created by matchmaker; else create lazily.
    if (!ctx->physics_world) {
        ctx->physics_world = std::make_unique<t2d::phys::World>(b2Vec2{0.0f, 0.0f});
    }
    auto &phys_world = *ctx->physics_world; // alias
    ProjectileMap projectile_bodies; // projectile id -> body id
    // Initialize physics body list
    phys_world.tank_bodies.clear();
    for (auto &adv : ctx->tanks) {
        phys_world.tank_bodies.push_back(adv.hull);
    }
    using clock = std::chrono::steady_clock;
    auto tick_interval = std::chrono::milliseconds(1000 / ctx->tick_rate);
    auto next = clock::now();
    while (true) {
        auto now = clock::now();
        if (now < next) {
            co_await scheduler->yield_for(next - now);
            continue;
        }
        auto tick_start = now;
        next += tick_interval;
        ctx->server_tick++;
        // Handle disconnects: identify players removed from session manager snapshot
        {
            // Build a set of active session_ids from session manager
            auto active_sessions = t2d::mm::instance().snapshot_all_sessions();
            std::unordered_set<std::string> active_ids;
            active_ids.reserve(active_sessions.size());
            for (auto &sp : active_sessions) {
                if (!sp->session_id.empty())
                    active_ids.insert(sp->session_id);
            }
            for (size_t i = 0; i < ctx->players.size(); ++i) {
                auto &sess = ctx->players[i];
                if (sess->is_bot)
                    continue; // bots persist until match end
                if (!sess->session_id.empty() && active_ids.find(sess->session_id) == active_ids.end()) {
                    // Session disconnected; mark tank dead (if not already) and queue removal if not yet recorded
                    if (i < ctx->tanks.size()) {
                        auto &tank = ctx->tanks[i];
                        if (tank.hp > 0) {
                            tank.hp = 0;
                            ctx->removed_tanks_since_full.push_back(tank.entity_id);
                            ctx->kill_feed_events.emplace_back(tank.entity_id, 0);
                            t2d::ServerMessage tdmsg;
                            auto *td = tdmsg.mutable_destroyed();
                            td->set_victim_id(tank.entity_id);
                            td->set_attacker_id(0); // environment / disconnect
                            for (auto &pl : ctx->players)
                                t2d::mm::instance().push_message(pl, tdmsg);
                        }
                    }
                }
            }
        }
        // Basic input-driven updates (no collision / bounds yet)
        if (ctx->reload_timers.size() != ctx->tanks.size()) {
            ctx->reload_timers.resize(ctx->tanks.size(), 0.f);
        }
        float dt = 1.0f / static_cast<float>(ctx->tick_rate);
        for (size_t i = 0; i < ctx->tanks.size() && i < ctx->players.size(); ++i) {
            auto &adv = ctx->tanks[i];
            if (adv.hp == 0)
                continue; // dead
            auto &sess = ctx->players[i];
            auto input = t2d::mm::instance().get_input_copy(sess);
            // Basic bot AI: if bot, synthesize movement & periodic fire
            if (sess->is_bot) {
                // Acquire current tank transform
                b2Transform myHull = b2Body_GetTransform(adv.hull);
                b2Transform myTurret = b2Body_GetTransform(adv.turret);
                float myHullRad = std::atan2(myHull.q.s, myHull.q.c);
                float myTurretRad = std::atan2(myTurret.q.s, myTurret.q.c);
                // Auto-target nearest alive non-bot (fallback to any other alive tank)
                int target_index = -1;
                float best_d2 = 1e30f;
                for (size_t j = 0; j < ctx->tanks.size(); ++j) {
                    if (j == i)
                        continue;
                    const auto &ot = ctx->tanks[j];
                    if (ot.hp == 0)
                        continue;
                    b2Transform oHull = b2Body_GetTransform(ot.hull);
                    float dx = oHull.p.x - myHull.p.x;
                    float dy = oHull.p.y - myHull.p.y;
                    float d2 = dx * dx + dy * dy;
                    if (!ctx->players[j]->is_bot) {
                        d2 *= 0.25f; // prefer real players
                    }
                    if (d2 < best_d2) {
                        best_d2 = d2;
                        target_index = (int)j;
                    }
                }
                float desired_rad = myHullRad; // default keep current
                float last_align_err = 9999.f;
                if (target_index >= 0) {
                    const auto &tt = ctx->tanks[target_index];
                    b2Transform ttHull = b2Body_GetTransform(tt.hull);
                    float dx = ttHull.p.x - myHull.p.x;
                    float dy = ttHull.p.y - myHull.p.y;
                    desired_rad = std::atan2(dy, dx);
                    float diff = desired_rad - myHullRad;
                    while (diff > (float)M_PI)
                        diff -= 2.f * (float)M_PI;
                    while (diff < -(float)M_PI)
                        diff += 2.f * (float)M_PI;
                    input.turn_dir = std::clamp(diff * 180.f / 90.f / (float)M_PI, -1.f, 1.f);
                    float dist2 = dx * dx + dy * dy;
                    if (dist2 > 25.f) {
                        input.move_dir = 1.0f;
                    } else if (dist2 < 9.f) {
                        input.move_dir = -0.4f;
                    } else {
                        input.move_dir = 0.2f;
                    }
                    float tdiff = desired_rad - myTurretRad;
                    while (tdiff > (float)M_PI)
                        tdiff -= 2.f * (float)M_PI;
                    while (tdiff < -(float)M_PI)
                        tdiff += 2.f * (float)M_PI;
                    last_align_err = std::fabs(tdiff) * 180.f / (float)M_PI;
                    if (std::fabs(tdiff) < 2.f * (float)M_PI / 180.f) {
                        input.turret_turn = 0.f;
                    } else {
                        input.turret_turn = std::clamp(tdiff * 180.f / (30.f * (float)M_PI), -1.f, 1.f);
                    }
                } else {
                    input.turn_dir = 0.2f;
                    input.move_dir = 0.0f;
                    input.turret_turn = 0.3f;
                }
                if (!ctx->disable_bot_fire) {
                    uint32_t interval = ctx->bot_fire_interval_ticks == 0 ? 1 : ctx->bot_fire_interval_ticks;
                    bool cadence = (ctx->server_tick % interval) == 0;
                    if (cadence) {
                        if (target_index >= 0) {
                            input.fire = last_align_err < 20.f; // relaxed alignment threshold
                        } else {
                            input.fire = false;
                        }
                    } else {
                        input.fire = false;
                    }
                } else {
                    input.fire = false;
                }
                t2d::mm::Session::InputState upd = input;
                t2d::mm::instance().set_bot_input(sess, upd);
            }
            // Advanced drive forces
            t2d::phys::TankDriveInput drive{};
            drive.drive_forward = std::clamp(input.move_dir, -1.f, 1.f);
            drive.turn = std::clamp(input.turn_dir, -1.f, 1.f);
            drive.brake = input.brake; // new brake input
            t2d::phys::apply_tracked_drive(drive, adv, dt);

            // Turret aim: accumulate turret angle (convert from degrees to target world angle incrementally)
            if (std::fabs(input.turret_turn) > 0.0001f) {
                // current turret world angle
                b2Transform xt = b2Body_GetTransform(adv.turret);
                float current = std::atan2(xt.q.s, xt.q.c);
                float desired =
                    current + input.turret_turn * t2d::game::turret_turn_speed_deg() * dt * float(M_PI / 180.0);
                t2d::phys::TurretAimInput aim{};
                aim.target_angle_world = desired;
                t2d::phys::update_turret_aim(aim, adv);
            }
            if (input.fire && adv.ammo > 0) {
                float forward_offset = 3.3f;
                auto pid = ctx->next_projectile_id++;
                // Use advanced firing (spawns projectile and applies cooldown/ammo)
                uint32_t fired = t2d::phys::fire_projectile_if_ready(
                    adv, phys_world, ctx->projectile_speed, ctx->projectile_density, forward_offset, pid);
                if (fired) {
                    // Record projectile meta for snapshots (position will sync from physics later)
                    // Get muzzle position from turret transform again
                    b2Transform xt = b2Body_GetTransform(adv.turret);
                    b2Vec2 dir{xt.q.c, xt.q.s};
                    b2Vec2 pos{xt.p.x + dir.x * forward_offset, xt.p.y + dir.y * forward_offset};
                    ctx->projectiles.push_back(
                        {fired,
                         pos.x,
                         pos.y,
                         dir.x * ctx->projectile_speed,
                         dir.y * ctx->projectile_speed,
                         adv.entity_id});
                    // map projectile body for collision processing
                    projectile_bodies.emplace(fired, phys_world.projectile_bodies.back());
                    if (sess->is_bot)
                        t2d::mm::instance().clear_bot_fire(sess);
                }
            }
            // Reload timer update
            auto &rt = ctx->reload_timers[i];
            if (adv.ammo < ctx->max_ammo) {
                rt += dt;
                if (rt >= ctx->reload_interval_sec) {
                    adv.ammo++;
                    rt = 0.f;
                }
            } else {
                rt = 0.f; // full ammo, keep timer reset
            }
        }
        for (auto &adv : ctx->tanks) {
            if (adv.fire_cooldown_cur > 0.f)
                adv.fire_cooldown_cur = std::max(0.f, adv.fire_cooldown_cur - dt);
        }
        // Physics step (tanks + projectiles) then process contacts before any projectile body destruction
        t2d::phys::step(phys_world, dt);
        // Handle projectile vs tank impacts (must run before bounds cull destroys bodies)
        process_contacts(phys_world, projectile_bodies, *ctx);
        // Sync tank state (position + angles) from advanced bodies
        // Angles & positions derived on snapshot build.
        // Sync projectile positions from physics bodies (remove invalid)
        for (auto &p : ctx->projectiles) {
            auto it = projectile_bodies.find(p.id);
            if (it != projectile_bodies.end()) {
                auto pos = t2d::phys::get_body_position(it->second);
                p.x = pos.x;
                p.y = pos.y;
            } else {
                // Fallback: if body missing keep manual update
                p.x += p.vx * dt;
                p.y += p.vy * dt;
            }
        }
        // Simple bounds cull for projectiles (world prototype area +/-100)
        {
            std::vector<size_t> to_remove_bounds;
            for (size_t i = 0; i < ctx->projectiles.size(); ++i) {
                auto &pr = ctx->projectiles[i];
                if (std::fabs(pr.x) > 100.f || std::fabs(pr.y) > 100.f) {
                    to_remove_bounds.push_back(i);
                }
            }
            if (!to_remove_bounds.empty()) {
                for (auto it = to_remove_bounds.rbegin(); it != to_remove_bounds.rend(); ++it) {
                    auto pid = ctx->projectiles[*it].id;
                    auto body_it = projectile_bodies.find(pid);
                    if (body_it != projectile_bodies.end()) {
                        t2d::phys::destroy_body(body_it->second);
                        projectile_bodies.erase(body_it);
                    }
                    ctx->removed_projectiles_since_full.push_back(pid);
                    ctx->projectiles.erase(ctx->projectiles.begin() + *it);
                }
            }
        }
        // (Contact processing already performed earlier this tick)
        if (ctx->snapshot_interval_ticks > 0 && ctx->server_tick % ctx->snapshot_interval_ticks == 0) {
            bool send_full = (ctx->server_tick - ctx->last_full_snapshot_tick >= ctx->full_snapshot_interval_ticks);
            if (send_full) {
                t2d::ServerMessage sm;
                auto *snap = sm.mutable_snapshot();
                snap->set_server_tick(static_cast<uint32_t>(ctx->server_tick));
                ctx->last_full_snapshot_tick = static_cast<uint32_t>(ctx->server_tick);
                // Rebuild cache from physics state
                ctx->last_sent_tanks.clear();
                ctx->last_sent_tanks.resize(ctx->tanks.size());
                for (size_t ti = 0; ti < ctx->tanks.size(); ++ti) {
                    auto &adv = ctx->tanks[ti];
                    if (adv.hp == 0)
                        continue;
                    auto *ts = snap->add_tanks();
                    ts->set_entity_id(adv.entity_id);
                    auto pos = t2d::phys::get_body_position(adv.hull);
                    b2Transform xh = b2Body_GetTransform(adv.hull);
                    b2Transform xt = b2Body_GetTransform(adv.turret);
                    float hull_rad = std::atan2(xh.q.s, xh.q.c) * 180.f / 3.14159265f;
                    float tur_rad = std::atan2(xt.q.s, xt.q.c) * 180.f / 3.14159265f;
#if T2D_ENABLE_SNAPSHOT_QUANT
                    // Quantize positions & angles into integer buckets stored still as float (prototype keeps proto
                    // schema unchanged)
                    constexpr float POS_SCALE = 100.f; // 1cm
                    constexpr float ANG_SCALE = 10.f; // 0.1 deg
                    ts->set_x(std::round(pos.x * POS_SCALE) / POS_SCALE);
                    ts->set_y(std::round(pos.y * POS_SCALE) / POS_SCALE);
                    ts->set_hull_angle(std::round(hull_rad * ANG_SCALE) / ANG_SCALE);
                    ts->set_turret_angle(std::round(tur_rad * ANG_SCALE) / ANG_SCALE);
#else
                    ts->set_x(pos.x);
                    ts->set_y(pos.y);
                    ts->set_hull_angle(hull_rad);
                    ts->set_turret_angle(tur_rad);
#endif
                    // update cache
                    auto &cache = ctx->last_sent_tanks[ti];
                    cache.entity_id = adv.entity_id;
                    cache.x = pos.x;
                    cache.y = pos.y;
                    cache.hull_angle = hull_rad;
                    cache.turret_angle = tur_rad;
                    cache.hp = adv.hp;
                    cache.ammo = adv.ammo;
                    cache.alive = adv.hp > 0;
                    ts->set_hp(adv.hp);
                    ts->set_ammo(adv.ammo);
                }
                // above loop sets hp/ammo, continue existing code path (skip duplicate at end)
                for (auto &adv_unused_for_scope : ctx->tanks) {
                    (void)adv_unused_for_scope; // no-op; maintain structure after refactor
                }
                for (auto &p : ctx->projectiles) {
                    auto *ps = snap->add_projectiles();
                    ps->set_projectile_id(p.id);
#if T2D_ENABLE_SNAPSHOT_QUANT
                    constexpr float POS_SCALE = 100.f;
                    ps->set_x(std::round(p.x * POS_SCALE) / POS_SCALE);
                    ps->set_y(std::round(p.y * POS_SCALE) / POS_SCALE);
                    ps->set_vx(p.vx); // velocities left unquantized for now
                    ps->set_vy(p.vy);
#else
                    ps->set_x(p.x);
                    ps->set_y(p.y);
                    ps->set_vx(p.vx);
                    ps->set_vy(p.vy);
#endif
                }
                // Approx size: serialized later per recipient; rough proto size estimation using string
                std::string tmp;
                sm.SerializeToString(&tmp);
                t2d::metrics::add_full(tmp.size());
#if T2D_ENABLE_SNAPSHOT_QUANT
                // Compression placeholder: RLE + optional zlib (only metrics currently recorded by rle_try/zlib_try)
                // Future: send compressed variant conditionally to clients advertising support.
#endif
                for (auto &pl : ctx->players)
                    t2d::mm::instance().push_message(pl, sm);
            } else {
                // delta snapshot
                t2d::ServerMessage sm;
                auto *delta = sm.mutable_delta_snapshot();
                delta->set_server_tick(static_cast<uint32_t>(ctx->server_tick));
                delta->set_base_tick(ctx->last_full_snapshot_tick);
                // compare tanks
                if (ctx->last_sent_tanks.size() != ctx->tanks.size())
                    ctx->last_sent_tanks.resize(ctx->tanks.size());
                for (size_t i = 0; i < ctx->tanks.size(); ++i) {
                    auto &adv = ctx->tanks[i];
                    if (adv.hp == 0)
                        continue;
                    if (i >= ctx->last_sent_tanks.size()) {
                        ctx->last_sent_tanks.push_back({adv.entity_id});
                    }
                    auto &prev = ctx->last_sent_tanks[i];
                    if (!prev.alive && adv.hp > 0) {
                        // resurrect case not expected in prototype; treat as changed
                    }
                    auto pos = t2d::phys::get_body_position(adv.hull);
                    b2Transform xh = b2Body_GetTransform(adv.hull);
                    b2Transform xt = b2Body_GetTransform(adv.turret);
                    float hull_deg = std::atan2(xh.q.s, xh.q.c) * 180.f / 3.14159265f;
                    float tur_deg = std::atan2(xt.q.s, xt.q.c) * 180.f / 3.14159265f;
                    bool changed = std::fabs(pos.x - prev.x) > 0.0001f || std::fabs(pos.y - prev.y) > 0.0001f
                        || std::fabs(hull_deg - prev.hull_angle) > 0.01f
                        || std::fabs(tur_deg - prev.turret_angle) > 0.01f || adv.hp != prev.hp || adv.ammo != prev.ammo;
                    if (changed) {
                        auto *ts = delta->add_tanks();
                        ts->set_entity_id(adv.entity_id);
#if T2D_ENABLE_SNAPSHOT_QUANT
                        constexpr float POS_SCALE = 100.f;
                        constexpr float ANG_SCALE = 10.f;
                        ts->set_x(std::round(pos.x * POS_SCALE) / POS_SCALE);
                        ts->set_y(std::round(pos.y * POS_SCALE) / POS_SCALE);
                        ts->set_hull_angle(std::round(hull_deg * ANG_SCALE) / ANG_SCALE);
                        ts->set_turret_angle(std::round(tur_deg * ANG_SCALE) / ANG_SCALE);
#else
                        ts->set_x(pos.x);
                        ts->set_y(pos.y);
                        ts->set_hull_angle(hull_deg);
                        ts->set_turret_angle(tur_deg);
#endif
                        ts->set_hp(adv.hp);
                        ts->set_ammo(adv.ammo);
                        prev.x = pos.x;
                        prev.y = pos.y;
                        prev.hull_angle = hull_deg;
                        prev.turret_angle = tur_deg;
                        prev.hp = adv.hp;
                        prev.ammo = adv.ammo;
                        prev.alive = adv.hp > 0;
                    }
                }
                for (auto id : ctx->removed_tanks_since_full)
                    delta->add_removed_tanks(id);
                // new projectiles since base: naive include all with id > 0 created after base tick (simplify: send all
                // projectiles whose id greater than count at last full snapshot) For prototype, just send all
                // projectiles (client would de-dup by id)
                for (auto &p : ctx->projectiles) {
                    auto *ps = delta->add_projectiles();
                    ps->set_projectile_id(p.id);
#if T2D_ENABLE_SNAPSHOT_QUANT
                    constexpr float POS_SCALE = 100.f;
                    ps->set_x(std::round(p.x * POS_SCALE) / POS_SCALE);
                    ps->set_y(std::round(p.y * POS_SCALE) / POS_SCALE);
                    ps->set_vx(p.vx);
                    ps->set_vy(p.vy);
#else
                    ps->set_x(p.x);
                    ps->set_y(p.y);
                    ps->set_vx(p.vx);
                    ps->set_vy(p.vy);
#endif
                }
                for (auto id : ctx->removed_projectiles_since_full)
                    delta->add_removed_projectiles(id);
                std::string tmp;
                sm.SerializeToString(&tmp);
                t2d::metrics::add_delta(tmp.size());
#if T2D_ENABLE_SNAPSHOT_QUANT
                // As above, compression logic lives in snapshot_compress.* (not applied to wire in prototype)
#endif
                for (auto &pl : ctx->players)
                    t2d::mm::instance().push_message(pl, sm);
            }
            if (send_full) {
                // Clear removed lists after full snapshot baseline
                ctx->removed_projectiles_since_full.clear();
                ctx->removed_tanks_since_full.clear();
            }
        }
        // Emit aggregated KillFeedUpdate if any events occurred this tick
        if (!ctx->kill_feed_events.empty()) {
            t2d::ServerMessage kfmsg;
            auto *kf = kfmsg.mutable_kill_feed();
            for (auto &e : ctx->kill_feed_events) {
                auto *ev = kf->add_events();
                ev->set_victim_id(e.first);
                ev->set_attacker_id(e.second);
            }
            for (auto &pl : ctx->players)
                t2d::mm::instance().push_message(pl, kfmsg);
            ctx->kill_feed_events.clear();
        }
        // Victory condition: only one (or zero) alive tank remains OR time limit reached.
        // Delay evaluation for initial warmup (avoid instant end when match starts with 1 player before bot fill, or
        // transient states).
        if (!ctx->match_over && ctx->server_tick > ctx->tick_rate * 2) { // ~2s grace period
            uint32_t alive_count = 0;
            uint32_t last_alive_id = 0;
            for (auto &t : ctx->tanks) {
                if (t.hp > 0) {
                    ++alive_count;
                    last_alive_id = t.entity_id;
                }
            }
            // If match started with only 1 player (all others bots), allow longer duration before auto-end.
            if (alive_count <= 1 && ctx->initial_player_count > 1) {
                ctx->match_over = true;
                ctx->winner_entity = last_alive_id;
            } else {
                // Base fallback timeout depends on configuration:
                //  - disable_bot_fire: extend greatly so player can observe movement without premature end
                //  - otherwise default 60s (was 30s previously)
                uint64_t fallback_ticks = ctx->tick_rate * (ctx->disable_bot_fire ? 300ull : 60ull);
                if (ctx->server_tick > fallback_ticks) {
                    ctx->match_over = true;
                }
            }
            if (ctx->match_over) {
                t2d::ServerMessage endmsg;
                auto *me = endmsg.mutable_match_end();
                me->set_match_id(ctx->match_id);
                me->set_winner_entity_id(ctx->winner_entity);
                me->set_server_tick(static_cast<uint32_t>(ctx->server_tick));
                for (auto &pl : ctx->players)
                    t2d::mm::instance().push_message(pl, endmsg);
                t2d::log::info("[match] over id={} winner_entity={}", ctx->match_id, ctx->winner_entity);
            }
        }
        // Extended max duration if single real player (avoid too-short session): 120s else 10s proto cap
        // Hard cap: ensure eventual termination. If only one real player (initial count 1), long cap.
        // Multi-player (or bots) sessions get larger cap now (60s normally, 300s if bot fire disabled).
        uint64_t hard_cap_ticks = (ctx->initial_player_count <= 1)
            ? (ctx->tick_rate * 120ull)
            : (ctx->tick_rate * (ctx->disable_bot_fire ? 300ull : 60ull));
        if (ctx->match_over || ctx->server_tick > hard_cap_ticks) {
            t2d::log::info("[match] end id={}", ctx->match_id);
            // Destroy remaining projectile bodies
            for (auto &kv : projectile_bodies) {
                t2d::phys::destroy_body(kv.second);
            }
            projectile_bodies.clear();
            // Adjust metrics on match end
            t2d::metrics::runtime().active_matches.fetch_sub(1, std::memory_order_relaxed);
            // bots_in_match is a gauge reflecting current bots across matches; subtract bots from this match
            size_t bots = 0;
            for (auto &pl : ctx->players)
                if (pl->is_bot)
                    ++bots;
            if (bots > 0)
                t2d::metrics::runtime().bots_in_match.fetch_sub(bots, std::memory_order_relaxed);
            co_return;
        }
        // Record runtime metrics
        t2d::metrics::runtime().projectiles_active.store(ctx->projectiles.size(), std::memory_order_relaxed);
        auto tick_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - tick_start).count();
        t2d::metrics::add_tick_duration(static_cast<uint64_t>(tick_ns));
    }
}

} // namespace t2d::game
