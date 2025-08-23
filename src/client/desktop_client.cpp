// SPDX-License-Identifier: Apache-2.0
#include "common/framing.hpp"
#include "common/logger.hpp"
#include "game.pb.h"

#include <coro/coro.hpp>
#include <coro/default_executor.hpp>
#include <coro/io_scheduler.hpp>
#include <coro/net/tcp/client.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

using namespace std::chrono_literals;

namespace {

// In-memory simplified world model for demonstration & basic reconciliation.
struct TankSimple
{
    uint32_t id{};
    float x{};
    float y{};
    float hull_angle{};
    float turret_angle{};
    uint32_t hp{};
    uint32_t ammo{};
};

struct ProjectileSimple
{
    uint32_t id{};
    float x{};
    float y{};
    float vx{};
    float vy{};
};

struct ClientWorld
{
    std::unordered_map<uint32_t, TankSimple> tanks;
    std::unordered_map<uint32_t, ProjectileSimple> projectiles;
    uint32_t last_full_tick{0};
    uint32_t last_tick{0};

    void apply_full(const t2d::StateSnapshot &snap)
    {
        tanks.clear();
        projectiles.clear();
        last_full_tick = snap.server_tick();
        last_tick = snap.server_tick();
        for (const auto &t : snap.tanks()) {
            tanks[t.entity_id()] =
                TankSimple{t.entity_id(), t.x(), t.y(), t.hull_angle(), t.turret_angle(), t.hp(), t.ammo()};
        }
        for (const auto &p : snap.projectiles()) {
            projectiles[p.projectile_id()] = ProjectileSimple{p.projectile_id(), p.x(), p.y(), p.vx(), p.vy()};
        }
    }

    void apply_delta(const t2d::DeltaSnapshot &d)
    {
        // Only apply if base tick matches our last_full_tick (simple guard)
        last_tick = d.server_tick();
        for (auto id : d.removed_tanks()) {
            tanks.erase(id);
        }
        for (auto id : d.removed_projectiles()) {
            projectiles.erase(id);
        }
        for (const auto &t : d.tanks()) {
            auto &ref = tanks[t.entity_id()];
            ref.id = t.entity_id();
            ref.x = t.x();
            ref.y = t.y();
            ref.hull_angle = t.hull_angle();
            ref.turret_angle = t.turret_angle();
            ref.hp = t.hp();
            ref.ammo = t.ammo();
        }
        for (const auto &p : d.projectiles()) {
            projectiles[p.projectile_id()] = ProjectileSimple{p.projectile_id(), p.x(), p.y(), p.vx(), p.vy()};
        }
    }
};

std::atomic_bool g_shutdown{false};

void handle_sig(int)
{
    g_shutdown.store(true);
}

coro::task<void> send_frame(coro::net::tcp::client &client, const t2d::ClientMessage &msg)
{
    co_await client.poll(coro::poll_op::write);
    std::string payload;
    if (!msg.SerializeToString(&payload))
        co_return;
    uint32_t len = htonl(static_cast<uint32_t>(payload.size()));
    std::string frame;
    frame.resize(4 + payload.size());
    std::memcpy(frame.data(), &len, 4);
    std::memcpy(frame.data() + 4, payload.data(), payload.size());
    std::span<const char> rest(frame.data(), frame.size());
    while (!rest.empty()) {
        co_await client.poll(coro::poll_op::write);
        auto [st, remaining] = client.send(rest);
        if (st == coro::net::send_status::ok || st == coro::net::send_status::would_block) {
            rest = remaining;
            continue;
        } else {
            co_return;
        }
    }
}

coro::task<bool> read_one(coro::net::tcp::client &client, t2d::ServerMessage &out)
{
    static t2d::netutil::FrameParseState state;
    co_await client.poll(coro::poll_op::read);
    std::string tmp(2048, '\0');
    auto [st, span] = client.recv(tmp);
    if (st == coro::net::recv_status::closed)
        co_return false;
    if (st != coro::net::recv_status::ok)
        co_return false;
    state.buffer.insert(state.buffer.end(), span.begin(), span.end());
    std::string payload;
    if (!t2d::netutil::try_extract(state, payload))
        co_return false;
    if (!out.ParseFromArray(payload.data(), (int)payload.size()))
        co_return false;
    co_return true;
}

coro::task<void> run_client(std::shared_ptr<coro::io_scheduler> sched, std::string host, uint16_t port)
{
    co_await sched->schedule();
    coro::net::tcp::client cli{sched, {.address = coro::net::ip_address::from_string(host), .port = port}};
    auto rc = co_await cli.connect(5s);
    if (rc != coro::net::connect_status::connected) {
        t2d::log::error("connect failed");
        co_return;
    }
    t2d::log::info("connected host={} port={}", host, port);
    // Auth request (stub token)
    t2d::ClientMessage auth;
    auto *rq = auth.mutable_auth_request();
    rq->set_oauth_token("desktop_dummy");
    rq->set_client_version(T2D_VERSION);
    co_await send_frame(cli, auth);
    // Queue join
    t2d::ClientMessage q;
    q.mutable_queue_join();
    co_await send_frame(cli, q);
    bool in_match = false;
    uint64_t loop_iter = 0;
    std::string session_id;
    ClientWorld world;
    while (!g_shutdown.load()) {
        // Periodic heartbeat every ~1s (assuming ~20ms yield below => ~50 iterations)
        if ((loop_iter % 50) == 0) {
            t2d::ClientMessage hb;
            auto *h = hb.mutable_heartbeat();
            h->set_session_id(session_id);
            h->set_time_ms((uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count());
            co_await send_frame(cli, hb);
        }
        // Send input while in match (simple circular motion + fire pulse)
        if (in_match && (loop_iter % 5) == 0) { // 100ms intervals
            t2d::ClientMessage in;
            auto *ic = in.mutable_input();
            ic->set_session_id(session_id);
            ic->set_client_tick((uint32_t)loop_iter);
            float phase = static_cast<float>(loop_iter % 360) * 3.14159f / 180.0f;
            ic->set_move_dir(std::sin(phase)); // oscillate forward/back
            ic->set_turn_dir(std::cos(phase)); // rotate hull
            ic->set_turret_turn(std::sin(phase * 0.5f));
            ic->set_fire((loop_iter % 150) == 0); // fire occasionally
            co_await send_frame(cli, in);
        }
        t2d::ServerMessage sm;
        if (co_await read_one(cli, sm)) {
            if (sm.has_auth_response()) {
                t2d::log::info(
                    "auth success={} session={}", sm.auth_response().success(), sm.auth_response().session_id());
                if (sm.auth_response().success()) {
                    session_id = sm.auth_response().session_id();
                }
            } else if (sm.has_queue_status()) {
                t2d::log::info(
                    "queue pos={} players={} need={} timeout_left={}",
                    sm.queue_status().position(),
                    sm.queue_status().players_in_queue(),
                    sm.queue_status().needed_for_match(),
                    sm.queue_status().timeout_seconds_left());
            } else if (sm.has_match_start()) {
                in_match = true;
                t2d::log::info(
                    "match start id={} tick_rate={} seed={}",
                    sm.match_start().match_id(),
                    sm.match_start().tick_rate(),
                    sm.match_start().seed());
            } else if (sm.has_snapshot()) {
                world.apply_full(sm.snapshot());
                t2d::log::info(
                    "full snapshot tick={} tanks={} projectiles={}",
                    sm.snapshot().server_tick(),
                    sm.snapshot().tanks_size(),
                    sm.snapshot().projectiles_size());
            } else if (sm.has_delta_snapshot()) {
                world.apply_delta(sm.delta_snapshot());
                t2d::log::debug(
                    "delta tick={} base={} dtanks={} dprojs={} removed_tanks={} removed_projs={}",
                    sm.delta_snapshot().server_tick(),
                    sm.delta_snapshot().base_tick(),
                    sm.delta_snapshot().tanks_size(),
                    sm.delta_snapshot().projectiles_size(),
                    sm.delta_snapshot().removed_tanks_size(),
                    sm.delta_snapshot().removed_projectiles_size());
            } else if (sm.has_damage()) {
                t2d::log::info(
                    "damage victim={} attacker={} hp_left={}",
                    sm.damage().victim_id(),
                    sm.damage().attacker_id(),
                    sm.damage().remaining_hp());
            } else if (sm.has_destroyed()) {
                t2d::log::info(
                    "tank destroyed victim={} attacker={}", sm.destroyed().victim_id(), sm.destroyed().attacker_id());
            } else if (sm.has_kill_feed()) {
                for (const auto &ev : sm.kill_feed().events()) {
                    t2d::log::info("kill feed event victim={} attacker={}", ev.victim_id(), ev.attacker_id());
                }
            } else if (sm.has_match_end()) {
                t2d::log::info(
                    "match end id={} winner_entity={}", sm.match_end().match_id(), sm.match_end().winner_entity_id());
            }
        }
        // Periodic world summary every ~1s
        if ((loop_iter % 50) == 0 && in_match) {
            size_t shown = 0;
            for (const auto &kv : world.tanks) {
                if (shown++ >= 3)
                    break;
                t2d::log::info(
                    "tank id={} pos=({:.2f},{:.2f}) hp={} ammo={} hull={:.2f} turret={:.2f}",
                    kv.second.id,
                    kv.second.x,
                    kv.second.y,
                    kv.second.hp,
                    kv.second.ammo,
                    kv.second.hull_angle,
                    kv.second.turret_angle);
            }
        }
        ++loop_iter;
        co_await sched->yield_for(20ms);
    }
    t2d::log::info("client shutdown");
}
} // namespace

int main(int argc, char **argv)
{
    std::string host = "127.0.0.1";
    uint16_t port = 40000;
    if (argc > 1)
        host = argv[1];
    if (argc > 2)
        port = static_cast<uint16_t>(std::stoi(argv[2]));
    std::signal(SIGINT, handle_sig);
    std::signal(SIGTERM, handle_sig);
    setenv("T2D_LOG_LEVEL", "info", 1);
    t2d::log::init();
    auto sched = coro::default_executor::io_executor();
    sched->spawn(run_client(sched, host, port));
    while (!g_shutdown.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return 0;
}
