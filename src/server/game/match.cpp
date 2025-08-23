#include "server/game/match.hpp"

#include "common/metrics.hpp"
#include "server/game/physics.hpp"

#include <cmath>
#include <iostream>
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
        if (!tank.alive)
            continue;
        auto pit =
            std::find_if(ctx.projectiles.begin(), ctx.projectiles.end(), [&](auto &p) { return p.id == proj_id; });
        if (pit == ctx.projectiles.end())
            continue;
        auto &proj = *pit;
        if (tank.entity_id == proj.owner)
            continue;
        if (tank.hp > 0) {
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
            if (tank.hp == 0) {
                tank.alive = false;
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
            t2d::phys::destroy_body(body_it->second);
            projectile_bodies.erase(body_it);
        }
        ctx.removed_projectiles_since_full.push_back(proj_id);
        ctx.projectiles.erase(pit);
    }
}
} // anonymous namespace

namespace t2d::game {

coro::task<void> run_match(std::shared_ptr<coro::io_scheduler> scheduler, std::shared_ptr<MatchContext> ctx)
{
    co_await scheduler->schedule();
    std::cout << "[match] start id=" << ctx->match_id << " players=" << ctx->players.size() << std::endl;
    // Phase 1 physics integration: create Box2D world and bodies mirroring current tanks
    t2d::phys::World phys_world({0.0f, 0.0f}); // no gravity for top-down
    ProjectileMap projectile_bodies; // projectile id -> body id
    if (phys_world.tank_bodies.size() != ctx->tanks.size()) {
        for (auto &t : ctx->tanks) {
            t2d::phys::create_tank(phys_world, t.x, t.y);
        }
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
                        if (tank.alive) {
                            tank.alive = false;
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
            auto &t = ctx->tanks[i];
            if (!t.alive)
                continue; // skip dead tanks
            auto &sess = ctx->players[i];
            auto input = t2d::mm::instance().get_input_copy(sess);
            // Basic bot AI: if bot, synthesize movement & periodic fire
            if (sess->is_bot) {
                input.turn_dir = 0.0f;
                input.move_dir = 0.0f;
                uint32_t interval = ctx->bot_fire_interval_ticks == 0 ? 1 : ctx->bot_fire_interval_ticks;
                input.fire = (ctx->server_tick % interval) == 0;
                t2d::mm::Session::InputState upd = input;
                t2d::mm::instance().set_bot_input(sess, upd);
            }
            // Apply turning
            t.hull_angle += input.turn_dir * turn_speed_deg() * dt;
            if (t.hull_angle < 0)
                t.hull_angle += 360.f;
            else if (t.hull_angle >= 360.f)
                t.hull_angle -= 360.f;
            t.turret_angle += input.turret_turn * turret_turn_speed_deg() * dt;
            if (t.turret_angle < 0)
                t.turret_angle += 360.f;
            else if (t.turret_angle >= 360.f)
                t.turret_angle -= 360.f;
            // Move forward/backward along hull direction (very naive)
            float rad = t.hull_angle * 3.14159265f / 180.f;
            // Apply velocity to physics body (phase 1: simple set linear velocity each tick)
            if (i < phys_world.tank_bodies.size()) {
                auto id = phys_world.tank_bodies[i];
                t2d::phys::set_body_velocity(
                    id,
                    std::cos(rad) * input.move_dir * ctx->movement_speed,
                    std::sin(rad) * input.move_dir * ctx->movement_speed);
            } else {
                t.x += std::cos(rad) * input.move_dir * ctx->movement_speed * dt;
                t.y += std::sin(rad) * input.move_dir * ctx->movement_speed * dt;
            }
            if (input.fire && t.ammo > 0) {
                float speed = ctx->projectile_speed;
                auto pid = ctx->next_projectile_id++;
                // Use current tank position (already synced) and offset spawn slightly forward to avoid immediate
                // self-collision logic
                float ox = t.x + std::cos(rad) * 0.6f;
                float oy = t.y + std::sin(rad) * 0.6f;
                ctx->projectiles.push_back({pid, ox, oy, std::cos(rad) * speed, std::sin(rad) * speed, t.entity_id});
                // Create physics body for projectile
                auto body =
                    t2d::phys::create_projectile(phys_world, t.x, t.y, std::cos(rad) * speed, std::sin(rad) * speed);
                projectile_bodies.emplace(pid, body);
                t.ammo--;
                if (sess->is_bot)
                    t2d::mm::instance().clear_bot_fire(sess);
            }
            // Reload timer update
            auto &rt = ctx->reload_timers[i];
            if (t.ammo < ctx->max_ammo) {
                rt += dt;
                if (rt >= ctx->reload_interval_sec) {
                    t.ammo++;
                    rt = 0.f;
                }
            } else {
                rt = 0.f; // full ammo, keep timer reset
            }
        }
        // Update projectiles
        // Physics step (tanks + future projectiles). Projectiles still manual for now.
        t2d::phys::step(phys_world, dt);
        // Sync tank positions back from physics bodies
        for (size_t i = 0; i < ctx->tanks.size() && i < phys_world.tank_bodies.size(); ++i) {
            auto id = phys_world.tank_bodies[i];
            auto pos = t2d::phys::get_body_position(id);
            ctx->tanks[i].x = pos.x;
            ctx->tanks[i].y = pos.y;
        }
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
        // Contact-based projectile vs tank collision using Box2D contact events
        process_contacts(phys_world, projectile_bodies, *ctx);
        if (ctx->snapshot_interval_ticks > 0 && ctx->server_tick % ctx->snapshot_interval_ticks == 0) {
            bool send_full = (ctx->server_tick - ctx->last_full_snapshot_tick >= ctx->full_snapshot_interval_ticks);
            if (send_full) {
                t2d::ServerMessage sm;
                auto *snap = sm.mutable_snapshot();
                snap->set_server_tick(static_cast<uint32_t>(ctx->server_tick));
                ctx->last_full_snapshot_tick = static_cast<uint32_t>(ctx->server_tick);
                ctx->last_sent_tanks = ctx->tanks; // replace cache
                for (auto &t : ctx->tanks) {
                    if (!t.alive)
                        continue; // skip removed tanks in new full snapshot
                    auto *ts = snap->add_tanks();
                    ts->set_entity_id(t.entity_id);
#if T2D_ENABLE_SNAPSHOT_QUANT
                    // Quantize positions & angles into integer buckets stored still as float (prototype keeps proto
                    // schema unchanged)
                    constexpr float POS_SCALE = 100.f; // 1cm
                    constexpr float ANG_SCALE = 10.f; // 0.1 deg
                    ts->set_x(std::round(t.x * POS_SCALE) / POS_SCALE);
                    ts->set_y(std::round(t.y * POS_SCALE) / POS_SCALE);
                    ts->set_hull_angle(std::round(t.hull_angle * ANG_SCALE) / ANG_SCALE);
                    ts->set_turret_angle(std::round(t.turret_angle * ANG_SCALE) / ANG_SCALE);
#else
                    ts->set_x(t.x);
                    ts->set_y(t.y);
                    ts->set_hull_angle(t.hull_angle);
                    ts->set_turret_angle(t.turret_angle);
#endif
                    ts->set_hp(t.hp);
                    ts->set_ammo(t.ammo);
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
                // Attempt simple RLE compression (prototype). If compressed smaller, record metric.
                {
                    extern std::string rle_try(const std::string &); // forward (unused fallback)
                }
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
                    auto &curr = ctx->tanks[i];
                    if (!curr.alive)
                        continue; // do not send changed state for dead tanks
                    if (i >= ctx->last_sent_tanks.size()) {
                        ctx->last_sent_tanks.push_back(curr);
                    }
                    auto &prev = ctx->last_sent_tanks[i];
                    if (!prev.alive && curr.alive) {
                        // resurrect case not expected in prototype; treat as changed
                    }
                    bool changed = std::fabs(curr.x - prev.x) > 0.0001f || std::fabs(curr.y - prev.y) > 0.0001f
                        || std::fabs(curr.hull_angle - prev.hull_angle) > 0.01f
                        || std::fabs(curr.turret_angle - prev.turret_angle) > 0.01f || curr.hp != prev.hp
                        || curr.ammo != prev.ammo;
                    if (changed) {
                        auto *ts = delta->add_tanks();
                        ts->set_entity_id(curr.entity_id);
#if T2D_ENABLE_SNAPSHOT_QUANT
                        constexpr float POS_SCALE = 100.f;
                        constexpr float ANG_SCALE = 10.f;
                        ts->set_x(std::round(curr.x * POS_SCALE) / POS_SCALE);
                        ts->set_y(std::round(curr.y * POS_SCALE) / POS_SCALE);
                        ts->set_hull_angle(std::round(curr.hull_angle * ANG_SCALE) / ANG_SCALE);
                        ts->set_turret_angle(std::round(curr.turret_angle * ANG_SCALE) / ANG_SCALE);
#else
                        ts->set_x(curr.x);
                        ts->set_y(curr.y);
                        ts->set_hull_angle(curr.hull_angle);
                        ts->set_turret_angle(curr.turret_angle);
#endif
                        ts->set_hp(curr.hp);
                        ts->set_ammo(curr.ammo);
                        prev = curr; // update cache
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
                {
                    extern std::string rle_try(const std::string &);
                }
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
                if (t.alive) {
                    ++alive_count;
                    last_alive_id = t.entity_id;
                }
            }
            if (alive_count <= 1) {
                ctx->match_over = true;
                ctx->winner_entity = last_alive_id;
            } else if (ctx->server_tick > ctx->tick_rate * 30) { // fallback timeout ~30s
                ctx->match_over = true;
            }
            if (ctx->match_over) {
                t2d::ServerMessage endmsg;
                auto *me = endmsg.mutable_match_end();
                me->set_match_id(ctx->match_id);
                me->set_winner_entity_id(ctx->winner_entity);
                me->set_server_tick(static_cast<uint32_t>(ctx->server_tick));
                for (auto &pl : ctx->players)
                    t2d::mm::instance().push_message(pl, endmsg);
                std::cout << "[match] over id=" << ctx->match_id << " winner_entity=" << ctx->winner_entity
                          << std::endl;
            }
        }
        if (ctx->match_over || ctx->server_tick > ctx->tick_rate * 10) {
            std::cout << "[match] end id=" << ctx->match_id << std::endl;
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
