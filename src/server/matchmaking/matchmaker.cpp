// SPDX-License-Identifier: Apache-2.0
#include "common/logger.hpp"
#include "common/metrics.hpp"
#include "game.pb.h"
#include "server/game/match.hpp"
#include "server/matchmaking/session_manager.hpp"

#include <coro/coro.hpp>

#include <iostream>
#include <random>

namespace t2d::mm {

struct MatchConfig
{
    uint32_t max_players;
    uint32_t fill_timeout_seconds; // after this we fill with bots
    uint32_t tick_rate;
    uint32_t poll_interval_ms{200};
    uint32_t snapshot_interval_ticks{5};
    uint32_t full_snapshot_interval_ticks{30};
    uint32_t bot_fire_interval_ticks{60};
    float movement_speed{2.0f};
    uint32_t projectile_damage{25};
    float reload_interval_sec{3.0f};
    float projectile_speed{5.0f};
};

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
                mgr.push_message(sess, smsg);
            }
        }
        if (queued.size() >= cfg.max_players) {
            // form match using first max_players
            std::vector<std::shared_ptr<Session>> group(queued.begin(), queued.begin() + cfg.max_players);
            mgr.pop_from_queue(group);
            uint32_t seed = random_seed();
            auto ctx = std::make_shared<t2d::game::MatchContext>();
            ctx->match_id = "m_" + std::to_string(seed);
            ctx->tick_rate = cfg.tick_rate;
            ctx->players = group;
            ctx->snapshot_interval_ticks = cfg.snapshot_interval_ticks;
            ctx->full_snapshot_interval_ticks = cfg.full_snapshot_interval_ticks;
            ctx->bot_fire_interval_ticks = cfg.bot_fire_interval_ticks;
            ctx->movement_speed = cfg.movement_speed;
            ctx->projectile_damage = cfg.projectile_damage;
            ctx->reload_interval_sec = cfg.reload_interval_sec;
            ctx->projectile_speed = cfg.projectile_speed;
            uint32_t eid = 1;
            for (auto &s : group) {
                t2d::game::TankStateSimple tank{eid++};
                // Deterministic initial placement prototype: first (real) player at origin, bots offset on -X looking
                // towards origin
                if (s->is_bot) {
                    // Place bots spaced along negative X axis to avoid instant projectile self-collisions
                    // entity_id starts at 1 for player, so bot entity_ids 2..N => positions -2,-3,...
                    tank.x = -static_cast<float>(tank.entity_id);
                    tank.y = 0.0f;
                    tank.hull_angle = 0.0f; // facing east toward player at origin
                    tank.turret_angle = 0.0f;
                } else {
                    // player stays at origin
                    tank.x = 0.0f;
                    tank.y = 0.0f;
                    tank.hull_angle = 0.0f;
                    tank.turret_angle = 0.0f;
                }
                ctx->tanks.push_back(tank);
                s->tank_entity_id = tank.entity_id;
                t2d::ServerMessage smsg;
                auto *ms = smsg.mutable_match_start();
                ms->set_match_id(ctx->match_id);
                ms->set_tick_rate(cfg.tick_rate);
                ms->set_seed(seed);
                mgr.push_message(s, smsg);
                t2d::log::info(std::string("MatchStart queued session=") + s->session_id);
            }
            // Baseline snapshot at server_tick=0 (only to real players)
            {
                t2d::ServerMessage base;
                auto *snap = base.mutable_snapshot();
                snap->set_server_tick(0);
                for (auto &t : ctx->tanks) {
                    auto *ts = snap->add_tanks();
                    ts->set_entity_id(t.entity_id);
                    ts->set_x(t.x);
                    ts->set_y(t.y);
                    ts->set_hull_angle(t.hull_angle);
                    ts->set_turret_angle(t.turret_angle);
                    ts->set_hp(t.hp);
                    ts->set_ammo(t.ammo);
                }
                ctx->last_full_snapshot_tick = 0; // baseline considered full base
                ctx->last_sent_tanks = ctx->tanks;
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
                t2d::metrics::runtime().active_matches.fetch_add(1, std::memory_order_relaxed);
                t2d::metrics::runtime().bots_in_match.fetch_add(bots, std::memory_order_relaxed);
            }
        }
        // TODO: partial match start after timeout with bots (future)
    }
}

} // namespace t2d::mm
