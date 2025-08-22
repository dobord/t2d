#include "server/game/match.hpp"

#include "common/metrics.hpp"

#include <cmath>
#include <iostream>

namespace t2d::game {

coro::task<void> run_match(std::shared_ptr<coro::io_scheduler> scheduler, std::shared_ptr<MatchContext> ctx)
{
    co_await scheduler->schedule();
    std::cout << "[match] start id=" << ctx->match_id << " players=" << ctx->players.size() << std::endl;
    using clock = std::chrono::steady_clock;
    auto tick_interval = std::chrono::milliseconds(1000 / ctx->tick_rate);
    auto next = clock::now();
    while (true) {
        auto now = clock::now();
        if (now < next) {
            co_await scheduler->yield_for(next - now);
            continue;
        }
        next += tick_interval;
        ctx->server_tick++;
        // Basic input-driven updates (no collision / bounds yet)
        float dt = 1.0f / static_cast<float>(ctx->tick_rate);
        for (size_t i = 0; i < ctx->tanks.size() && i < ctx->players.size(); ++i) {
            auto &t = ctx->tanks[i];
            auto &sess = ctx->players[i];
            auto input = t2d::mm::instance().get_input_copy(sess);
            // Basic bot AI: if bot, synthesize movement & periodic fire
            if (sess->is_bot) {
                // rotate slowly and move forward
                input.turn_dir = 0.2f;
                input.move_dir = 0.5f;
                // fire every ~2 seconds
                if (ctx->server_tick == 1 || ctx->server_tick % (ctx->tick_rate * 2) == 0) {
                    input.fire = true;
                }
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
            t.x += std::cos(rad) * input.move_dir * movement_speed() * dt;
            t.y += std::sin(rad) * input.move_dir * movement_speed() * dt;
            if (input.fire && t.ammo > 0) {
                float speed = 5.0f;
                ctx->projectiles.push_back(
                    {ctx->next_projectile_id++, t.x, t.y, std::cos(rad) * speed, std::sin(rad) * speed, t.entity_id});
                t.ammo--;
                if (sess->is_bot)
                    t2d::mm::instance().clear_bot_fire(sess);
            }
        }
        // Update projectiles
        for (auto &p : ctx->projectiles) {
            p.x += p.vx * dt;
            p.y += p.vy * dt;
        }
        // Collision detection (very naive circle overlap) -> DamageEvent
        {
            const float hit_radius = 0.3f; // prototype radius
            const uint32_t damage_amount = 25;
            // Collect indices to remove after processing to avoid iterator invalidation
            std::vector<size_t> remove_indices;
            for (size_t pi = 0; pi < ctx->projectiles.size(); ++pi) {
                auto &proj = ctx->projectiles[pi];
                bool hit = false;
                for (auto &tank : ctx->tanks) {
                    if (tank.entity_id == proj.owner)
                        continue; // no self-hit prototype
                    float dx = proj.x - tank.x;
                    float dy = proj.y - tank.y;
                    float dist2 = dx * dx + dy * dy;
                    if (dist2 <= hit_radius * hit_radius && tank.hp > 0) {
                        // Apply damage
                        if (tank.hp <= damage_amount)
                            tank.hp = 0;
                        else
                            tank.hp -= damage_amount;
                        // Emit DamageEvent
                        t2d::ServerMessage ev;
                        auto *d = ev.mutable_damage();
                        d->set_victim_id(tank.entity_id);
                        d->set_attacker_id(proj.owner);
                        d->set_amount(damage_amount);
                        d->set_remaining_hp(tank.hp);
                        std::cout << "[match] DamageEvent victim=" << tank.entity_id << " attacker=" << proj.owner
                                  << " hp=" << tank.hp << std::endl;
                        for (auto &pl : ctx->players)
                            t2d::mm::instance().push_message(pl, ev);
                        // If destroyed emit TankDestroyed (entity removal handled in later epic)
                        if (tank.hp == 0) {
                            t2d::ServerMessage tdmsg;
                            auto *td = tdmsg.mutable_destroyed();
                            td->set_victim_id(tank.entity_id);
                            td->set_attacker_id(proj.owner);
                            for (auto &pl : ctx->players)
                                t2d::mm::instance().push_message(pl, tdmsg);
                        }
                        hit = true;
                        break; // stop checking tanks for this projectile
                    }
                }
                if (hit)
                    remove_indices.push_back(pi);
            }
            if (!remove_indices.empty()) {
                // Remove in reverse order
                for (auto it = remove_indices.rbegin(); it != remove_indices.rend(); ++it) {
                    ctx->removed_projectiles_since_full.push_back(ctx->projectiles[*it].id);
                    ctx->projectiles.erase(ctx->projectiles.begin() + *it);
                }
            }
        }
        if (ctx->snapshot_interval_ticks > 0 && ctx->server_tick % ctx->snapshot_interval_ticks == 0) {
            bool send_full = (ctx->server_tick - ctx->last_full_snapshot_tick >= ctx->full_snapshot_interval_ticks);
            if (send_full) {
                t2d::ServerMessage sm;
                auto *snap = sm.mutable_snapshot();
                snap->set_server_tick(static_cast<uint32_t>(ctx->server_tick));
                ctx->last_full_snapshot_tick = static_cast<uint32_t>(ctx->server_tick);
                ctx->last_sent_tanks = ctx->tanks; // replace cache
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
                for (auto &p : ctx->projectiles) {
                    auto *ps = snap->add_projectiles();
                    ps->set_projectile_id(p.id);
                    ps->set_x(p.x);
                    ps->set_y(p.y);
                    ps->set_vx(p.vx);
                    ps->set_vy(p.vy);
                }
                // Approx size: serialized later per recipient; rough proto size estimation using string
                std::string tmp;
                sm.SerializeToString(&tmp);
                t2d::metrics::add_full(tmp.size());
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
                    if (i >= ctx->last_sent_tanks.size()) {
                        ctx->last_sent_tanks.push_back(curr);
                    }
                    auto &prev = ctx->last_sent_tanks[i];
                    bool changed = std::fabs(curr.x - prev.x) > 0.0001f || std::fabs(curr.y - prev.y) > 0.0001f
                        || std::fabs(curr.hull_angle - prev.hull_angle) > 0.01f
                        || std::fabs(curr.turret_angle - prev.turret_angle) > 0.01f || curr.hp != prev.hp
                        || curr.ammo != prev.ammo;
                    if (changed) {
                        auto *ts = delta->add_tanks();
                        ts->set_entity_id(curr.entity_id);
                        ts->set_x(curr.x);
                        ts->set_y(curr.y);
                        ts->set_hull_angle(curr.hull_angle);
                        ts->set_turret_angle(curr.turret_angle);
                        ts->set_hp(curr.hp);
                        ts->set_ammo(curr.ammo);
                        prev = curr; // update cache
                    }
                }
                // new projectiles since base: naive include all with id > 0 created after base tick (simplify: send all
                // projectiles whose id greater than count at last full snapshot) For prototype, just send all
                // projectiles (client would de-dup by id)
                for (auto &p : ctx->projectiles) {
                    auto *ps = delta->add_projectiles();
                    ps->set_projectile_id(p.id);
                    ps->set_x(p.x);
                    ps->set_y(p.y);
                    ps->set_vx(p.vx);
                    ps->set_vy(p.vy);
                }
                for (auto id : ctx->removed_projectiles_since_full)
                    delta->add_removed_projectiles(id);
                std::string tmp;
                sm.SerializeToString(&tmp);
                t2d::metrics::add_delta(tmp.size());
                for (auto &pl : ctx->players)
                    t2d::mm::instance().push_message(pl, sm);
            }
            if (send_full) {
                // Clear removed lists after full snapshot baseline
                ctx->removed_projectiles_since_full.clear();
                ctx->removed_tanks_since_full.clear();
            }
        }
        if (ctx->server_tick > ctx->tick_rate * 10) {
            std::cout << "[match] end id=" << ctx->match_id << std::endl;
            co_return;
        }
    }
}

} // namespace t2d::game
