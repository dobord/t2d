// SPDX-License-Identifier: Apache-2.0
#include "server/matchmaking/matchmaker.hpp"

#include "common/logger.hpp"
#include "common/metrics.hpp"
#include "game.pb.h"
#include "server/game/match.hpp"
#include "server/matchmaking/session_manager.hpp"

#include <coro/coro.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace t2d::mm {

// MatchConfig defined in header

static uint32_t random_seed()
{
    static std::mt19937 rng(std::random_device{}());
    return rng();
}

coro::task<void> run_matchmaker(std::shared_ptr<coro::io_scheduler> scheduler, MatchConfig cfg)
{
    co_await scheduler->schedule();
    t2d::log::info("matchmaker started");
    auto &mgr = instance();
    while (true) {
        // sleep configured poll interval
        co_await scheduler->yield_for(std::chrono::milliseconds(cfg.poll_interval_ms));
        auto queued = mgr.snapshot_queue();
        t2d::metrics::runtime().queue_depth.store(queued.size(), std::memory_order_relaxed);
        // Determine earliest join order and compute countdown time left for display.
        std::chrono::steady_clock::time_point earliest{};
        if (!queued.empty()) {
            earliest = queued.front()->queue_join_time;
            for (auto &q : queued)
                if (q->queue_join_time < earliest)
                    earliest = q->queue_join_time;
        }
        // Dynamic staged bot pacing: gradually add bots at 25%, 50%, 75%, 100% of timeout to reduce sudden fill.
        if (!queued.empty() && queued.size() < cfg.max_players && cfg.fill_timeout_seconds > 0) {
            auto waited =
                std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - earliest).count();
            double frac = static_cast<double>(waited) / static_cast<double>(cfg.fill_timeout_seconds);
            // Target minimum population fraction based on elapsed fraction of timeout
            double target_pop_frac = 0.0;
            if (frac >= 1.0) {
                target_pop_frac = 1.0; // full fill
            } else if (frac >= 0.75) {
                target_pop_frac = 0.75; // ensure 75%
            } else if (frac >= 0.5) {
                target_pop_frac = 0.5;
            } else if (frac >= 0.25) {
                target_pop_frac = 0.25;
            }
            size_t target_count = static_cast<size_t>(std::ceil(target_pop_frac * cfg.max_players));
            if (target_count > cfg.max_players)
                target_count = cfg.max_players;
            if (queued.size() < target_count) {
                size_t need = target_count - queued.size();
                if (need > 0) {
                    mgr.create_bots(need);
                    queued = mgr.snapshot_queue();
                }
            }
            // Final full bot fill at/after timeout if still short
            if (frac >= 1.0 && queued.size() < cfg.max_players) {
                size_t need = cfg.max_players - queued.size();
                mgr.create_bots(need);
                queued = mgr.snapshot_queue();
            }
        }

        // Periodic QueueStatusUpdate broadcast to all waiting sessions (real players only; bots don't receive msgs)
        if (!queued.empty()) {
            uint32_t players_now = static_cast<uint32_t>(queued.size());
            uint32_t lobby_countdown = 0;
            uint32_t projected_bot_fill = 0;
            if (cfg.fill_timeout_seconds > 0 && earliest.time_since_epoch().count() != 0) {
                auto waited =
                    std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - earliest)
                        .count();
                if (waited < 0)
                    waited = 0;
                if (waited >= static_cast<int64_t>(cfg.fill_timeout_seconds))
                    lobby_countdown = 0;
                else
                    lobby_countdown = cfg.fill_timeout_seconds - static_cast<uint32_t>(waited);
                if (players_now < cfg.max_players) {
                    projected_bot_fill = cfg.max_players - players_now;
                }
            }
            for (size_t i = 0; i < queued.size(); ++i) {
                auto &sess = queued[i];
                if (sess->is_bot)
                    continue;
                t2d::ServerMessage smsg;
                auto *qs = smsg.mutable_queue_status();
                qs->set_position(static_cast<uint32_t>(i + 1));
                qs->set_players_in_queue(players_now);
                qs->set_needed_for_match(cfg.max_players > players_now ? cfg.max_players - players_now : 0);
                qs->set_timeout_seconds_left(lobby_countdown);
                qs->set_lobby_countdown(lobby_countdown);
                qs->set_projected_bot_fill(projected_bot_fill);
                qs->set_lobby_state(0); // queued
                mgr.push_message(sess, smsg);
            }
        }
        if (queued.size() >= cfg.max_players) {
            // form match using first max_players
            std::vector<std::shared_ptr<Session>> group(queued.begin(), queued.begin() + cfg.max_players);
            mgr.pop_from_queue(group);
            uint32_t seed = cfg.fixed_seed > 0 ? cfg.fixed_seed : random_seed();
            auto ctx = std::make_shared<t2d::game::MatchContext>();
            ctx->match_id = "m_" + std::to_string(seed);
            ctx->tick_rate = cfg.tick_rate;
            ctx->players = group;
            ctx->initial_player_count = static_cast<uint32_t>(group.size());
            ctx->snapshot_interval_ticks = cfg.snapshot_interval_ticks;
            ctx->full_snapshot_interval_ticks = cfg.full_snapshot_interval_ticks;
            // For tests we want rapid engagements; clamp bot fire interval to <=5 ticks
            if (cfg.test_mode) {
                ctx->bot_fire_interval_ticks = std::min<uint32_t>(cfg.bot_fire_interval_ticks, 5u);
            } else {
                ctx->bot_fire_interval_ticks = cfg.bot_fire_interval_ticks;
            }
            ctx->movement_speed = cfg.movement_speed;
            // Boost projectile damage to ensure lethal within test timeout (>=50 overrides default if lower)
            ctx->projectile_damage = cfg.test_mode ? std::max<uint32_t>(cfg.projectile_damage, 50u)
                                                   : cfg.projectile_damage;
            ctx->reload_interval_sec = cfg.reload_interval_sec;
            ctx->projectile_speed = cfg.projectile_speed;
            ctx->projectile_density = cfg.projectile_density;
            ctx->projectile_max_lifetime_sec = cfg.projectile_max_lifetime_sec;
            ctx->fire_cooldown_sec = cfg.fire_cooldown_sec;
            ctx->hull_density = cfg.hull_density;
            ctx->turret_density = cfg.turret_density;
            ctx->disable_bot_fire = cfg.disable_bot_fire;
            ctx->disable_bot_ai = cfg.disable_bot_ai;
            ctx->test_mode = cfg.test_mode;
            ctx->map_width = cfg.map_width;
            ctx->map_height = cfg.map_height;
            ctx->persist_destroyed_tanks = cfg.persist_destroyed_tanks;
            ctx->track_break_hits = cfg.track_break_hits;
            ctx->turret_disable_front_hits = cfg.turret_disable_front_hits;
            ctx->physics_world = std::make_unique<t2d::phys::World>(b2Vec2{0.f, 0.f});
            // Spawn distribution (random or forced line for tests)
            uint32_t eid = 1;
            if (cfg.force_line_spawn) {
                // Evenly spaced along X axis centered at origin; y=0. Tanks face default orientation.
                float spacing = 20.f;
                size_t n = group.size();
                float start = -((static_cast<float>(n) - 1.f) * spacing * 0.5f);
                size_t idx = 0;
                for (auto &s : group) {
                    float x = start + spacing * static_cast<float>(idx++);
                    float y = 0.f;
                    auto phys_tank = t2d::phys::create_tank_with_turret(
                        *ctx->physics_world, x, y, eid++, ctx->hull_density, ctx->turret_density);
                    ctx->tanks.push_back(phys_tank);
                    s->tank_entity_id = phys_tank.entity_id;
                    t2d::ServerMessage smsg;
                    auto *ms = smsg.mutable_match_start();
                    ms->set_match_id(ctx->match_id);
                    ms->set_tick_rate(cfg.tick_rate);
                    ms->set_seed(seed);
                    ms->set_initial_player_count(static_cast<uint32_t>(group.size()));
                    ms->set_disable_bot_fire(cfg.disable_bot_fire);
                    ms->set_my_entity_id(s->tank_entity_id);
                    mgr.push_message(s, smsg);
                    t2d::log::info(std::string("MatchStart queued session=") + s->session_id);
                }
            } else {
                // Random non-overlapping spawn distribution inside map bounds.
                // Simple Poisson-like rejection sampling with limited attempts.
                std::mt19937 rng(seed);
                float safe_half_w = std::max(1.f, ctx->map_width * 0.5f - 5.f);
                float safe_half_h = std::max(1.f, ctx->map_height * 0.5f - 5.f);
                std::uniform_real_distribution<float> dx(-safe_half_w, safe_half_w);
                std::uniform_real_distribution<float> dy(-safe_half_h, safe_half_h);
                const float min_dist = 12.f; // separation to avoid overlap (tank ~6 world units long incl. turret)
                std::vector<std::pair<float, float>> placed;
                placed.reserve(group.size());
                for (auto &s : group) {
                    float x = 0.f, y = 0.f;
                    bool ok = false;
                    for (int attempt = 0; attempt < 200 && !ok; ++attempt) {
                        x = dx(rng);
                        y = dy(rng);
                        ok = true;
                        for (auto &pp : placed) {
                            float ddx = x - pp.first;
                            float ddy = y - pp.second;
                            if ((ddx * ddx + ddy * ddy) < (min_dist * min_dist)) {
                                ok = false;
                                break;
                            }
                        }
                    }
                    if (!ok) {
                        // fallback spiral placement
                        x = static_cast<float>(placed.size()) * 6.f;
                        y = 0.f;
                    }
                    placed.emplace_back(x, y);
                    auto phys_tank = t2d::phys::create_tank_with_turret(
                        *ctx->physics_world, x, y, eid++, ctx->hull_density, ctx->turret_density);
                    ctx->tanks.push_back(phys_tank);
                    s->tank_entity_id = phys_tank.entity_id;
                    t2d::ServerMessage smsg;
                    auto *ms = smsg.mutable_match_start();
                    ms->set_match_id(ctx->match_id);
                    ms->set_tick_rate(cfg.tick_rate);
                    ms->set_seed(seed);
                    ms->set_initial_player_count(static_cast<uint32_t>(group.size()));
                    ms->set_disable_bot_fire(cfg.disable_bot_fire);
                    ms->set_my_entity_id(s->tank_entity_id);
                    mgr.push_message(s, smsg);
                    t2d::log::info(std::string("MatchStart queued session=") + s->session_id);
                }
            }
            // Baseline snapshot at server_tick=0 (only to real players)
            {
                t2d::ServerMessage base;
                auto *snap = base.mutable_snapshot();
                snap->set_server_tick(0);
                // Include static map dimensions so clients can render boundaries
                snap->set_map_width(ctx->map_width);
                snap->set_map_height(ctx->map_height);
                for (auto &adv : ctx->tanks) {
                    if (adv.hp == 0)
                        continue;
                    auto *ts = snap->add_tanks();
                    auto pos = t2d::phys::get_body_position(adv.hull);
                    b2Transform xh = b2Body_GetTransform(adv.hull);
                    b2Transform xt = b2Body_GetTransform(adv.turret);
                    float hull_deg = std::atan2(xh.q.s, xh.q.c) * 180.f / 3.14159265f;
                    float tur_deg = std::atan2(xt.q.s, xt.q.c) * 180.f / 3.14159265f;
                    ts->set_entity_id(adv.entity_id);
                    ts->set_x(pos.x);
                    ts->set_y(pos.y);
                    ts->set_hull_angle(hull_deg);
                    ts->set_turret_angle(tur_deg);
                    ts->set_hp(adv.hp);
                    ts->set_ammo(adv.ammo);
                    ts->set_track_left_broken(adv.left_track_broken);
                    ts->set_track_right_broken(adv.right_track_broken);
                    ts->set_turret_disabled(adv.turret_disabled);
                    ctx->last_sent_tanks.push_back(
                        {adv.entity_id, pos.x, pos.y, hull_deg, tur_deg, adv.hp, adv.ammo, true});
                }
                ctx->last_full_snapshot_tick = 0;
                for (auto &s : group)
                    if (!s->is_bot)
                        mgr.push_message(s, base);
            }
            scheduler->spawn(t2d::game::run_match(scheduler, ctx));
            {
                t2d::log::info(std::string("match created players=") + std::to_string(group.size()));
                // Update metrics: count bots in this match
                size_t bots = 0;
                for (auto &s : group)
                    if (s->is_bot)
                        ++bots;
                auto &rt_reset = t2d::metrics::runtime();
                uint64_t prev_active = rt_reset.active_matches.fetch_add(1, std::memory_order_relaxed);
                rt_reset.bots_in_match.fetch_add(bots, std::memory_order_relaxed);
                if (prev_active == 0) {
                    // Zero out wait histogram & counters (raw + derived) to start steady-state accumulation.
                    rt_reset.wait_duration_ns_accum.store(0, std::memory_order_relaxed);
                    rt_reset.wait_samples.store(0, std::memory_order_relaxed);
                    for (int bi = 0; bi < t2d::metrics::RuntimeCounters::TICK_BUCKETS; ++bi) {
                        rt_reset.wait_hist[bi].store(0, std::memory_order_relaxed);
                    }
                }
            }
        }
        // TODO: partial match start after timeout with bots (future)
    } // while (true)
} // run_matchmaker

} // namespace t2d::mm
