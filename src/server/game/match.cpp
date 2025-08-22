#include "server/game/match.hpp"
#include <iostream>
#include <cmath>

namespace t2d::game {

coro::task<void> run_match(std::shared_ptr<coro::io_scheduler> scheduler, std::shared_ptr<MatchContext> ctx) {
    co_await scheduler->schedule();
    std::cout << "[match] start id=" << ctx->match_id << " players=" << ctx->players.size() << std::endl;
    using clock = std::chrono::steady_clock;
    auto tick_interval = std::chrono::milliseconds(1000 / ctx->tick_rate);
    auto next = clock::now();
    while(true) {
        auto now = clock::now();
        if(now < next) {
            co_await scheduler->yield_for(next - now);
            continue;
        }
        next += tick_interval;
        ctx->server_tick++;
        // Basic input-driven updates (no collision / bounds yet)
        float dt = 1.0f / static_cast<float>(ctx->tick_rate);
        for(size_t i=0;i<ctx->tanks.size() && i<ctx->players.size();++i) {
            auto &t = ctx->tanks[i];
            auto &sess = ctx->players[i];
            auto input = t2d::mm::instance().get_input_copy(sess);
            // Basic bot AI: if bot, synthesize movement & periodic fire
            if(sess->is_bot) {
                // rotate slowly and move forward
                input.turn_dir = 0.2f;
                input.move_dir = 0.5f;
                // fire every ~2 seconds
                if(ctx->server_tick % (ctx->tick_rate * 2) == 0) {
                    input.fire = true;
                }
                t2d::mm::Session::InputState upd = input;
                t2d::mm::instance().set_bot_input(sess, upd);
            }
            // Apply turning
            t.hull_angle += input.turn_dir * turn_speed_deg() * dt;
            if(t.hull_angle < 0) t.hull_angle += 360.f; else if(t.hull_angle >= 360.f) t.hull_angle -= 360.f;
            t.turret_angle += input.turret_turn * turret_turn_speed_deg() * dt;
            if(t.turret_angle < 0) t.turret_angle += 360.f; else if(t.turret_angle >= 360.f) t.turret_angle -= 360.f;
            // Move forward/backward along hull direction (very naive)
            float rad = t.hull_angle * 3.14159265f / 180.f;
            t.x += std::cos(rad) * input.move_dir * movement_speed() * dt;
            t.y += std::sin(rad) * input.move_dir * movement_speed() * dt;
            if(input.fire && t.ammo > 0) {
                float speed = 5.0f;
                ctx->projectiles.push_back({ctx->next_projectile_id++, t.x, t.y, std::cos(rad)*speed, std::sin(rad)*speed, t.entity_id});
                t.ammo--;
                if(sess->is_bot) t2d::mm::instance().clear_bot_fire(sess);
            }
        }
        // Update projectiles
        for(auto &p : ctx->projectiles) {
            p.x += p.vx * dt;
            p.y += p.vy * dt;
        }
        if(ctx->server_tick % 5 == 0) {
            bool send_full = (ctx->server_tick - ctx->last_full_snapshot_tick >= 30);
            if(send_full) {
                t2d::ServerMessage sm;
                auto *snap = sm.mutable_snapshot();
                snap->set_server_tick(static_cast<uint32_t>(ctx->server_tick));
                ctx->last_full_snapshot_tick = static_cast<uint32_t>(ctx->server_tick);
                ctx->last_sent_tanks = ctx->tanks; // replace cache
                for(auto &t : ctx->tanks) {
                    auto *ts = snap->add_tanks();
                    ts->set_entity_id(t.entity_id);
                    ts->set_x(t.x);
                    ts->set_y(t.y);
                    ts->set_hull_angle(t.hull_angle);
                    ts->set_turret_angle(t.turret_angle);
                    ts->set_hp(t.hp);
                    ts->set_ammo(t.ammo);
                }
                for(auto &p : ctx->projectiles) {
                    auto *ps = snap->add_projectiles();
                    ps->set_projectile_id(p.id);
                    ps->set_x(p.x);
                    ps->set_y(p.y);
                    ps->set_vx(p.vx);
                    ps->set_vy(p.vy);
                }
                for(auto &pl : ctx->players) t2d::mm::instance().push_message(pl, sm);
            } else {
                // delta snapshot
                t2d::ServerMessage sm;
                auto *delta = sm.mutable_delta_snapshot();
                delta->set_server_tick(static_cast<uint32_t>(ctx->server_tick));
                delta->set_base_tick(ctx->last_full_snapshot_tick);
                // compare tanks
                if(ctx->last_sent_tanks.size() != ctx->tanks.size()) ctx->last_sent_tanks.resize(ctx->tanks.size());
                for(size_t i=0;i<ctx->tanks.size();++i) {
                    auto &curr = ctx->tanks[i];
                    if(i >= ctx->last_sent_tanks.size()) { ctx->last_sent_tanks.push_back(curr); }
                    auto &prev = ctx->last_sent_tanks[i];
                    bool changed = std::fabs(curr.x - prev.x) > 0.0001f || std::fabs(curr.y - prev.y) > 0.0001f ||
                                   std::fabs(curr.hull_angle - prev.hull_angle) > 0.01f || std::fabs(curr.turret_angle - prev.turret_angle) > 0.01f ||
                                   curr.hp != prev.hp || curr.ammo != prev.ammo;
                    if(changed) {
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
                // new projectiles since base: naive include all with id > 0 created after base tick (simplify: send all projectiles whose id greater than count at last full snapshot)
                // For prototype, just send all projectiles (client would de-dup by id)
                for(auto &p : ctx->projectiles) {
                    auto *ps = delta->add_projectiles();
                    ps->set_projectile_id(p.id);
                    ps->set_x(p.x);
                    ps->set_y(p.y);
                    ps->set_vx(p.vx);
                    ps->set_vy(p.vy);
                }
                for(auto &pl : ctx->players) t2d::mm::instance().push_message(pl, sm);
            }
        }
        if(ctx->server_tick > ctx->tick_rate * 10) {
            std::cout << "[match] end id=" << ctx->match_id << std::endl;
            co_return;
        }
    }
}

} // namespace t2d::game
