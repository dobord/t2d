#include "common/logger.hpp"
#include "common/metrics.hpp"
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
        // If not enough real players but timeout exceeded for earliest player, fill with bots
        if (!queued.empty() && queued.size() < cfg.max_players) {
            // find earliest join time
            auto earliest = queued.front()->queue_join_time;
            for (auto &q : queued)
                if (q->queue_join_time < earliest)
                    earliest = q->queue_join_time;
            auto waited =
                std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - earliest).count();
            if (waited >= cfg.fill_timeout_seconds) {
                size_t need = cfg.max_players - queued.size();
                mgr.create_bots(need);
                queued = mgr.snapshot_queue();
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
            uint32_t eid = 1;
            for (auto &s : group) {
                t2d::game::TankStateSimple tank{eid++};
                // Deterministic initial placement prototype: first (real) player at origin, bots offset on -X looking
                // towards origin
                if (s->is_bot) {
                    // place bot at -1,0 facing east (0 degrees) so projectiles travel toward player tank at origin
                    tank.x = -1.0f;
                    tank.y = 0.0f;
                    tank.hull_angle = 0.0f;
                    tank.turret_angle = 0.0f;
                } else {
                    // player stays at origin
                    tank.x = 0.0f;
                    tank.y = 0.0f;
                    tank.hull_angle = 0.0f;
                    tank.turret_angle = 0.0f;
                }
                ctx->tanks.push_back(tank);
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
