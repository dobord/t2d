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
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {

// Option 2: No client-side world reconstruction / prediction. We just log raw snapshot & delta contents.
static void log_full_snapshot(const t2d::StateSnapshot &snap)
{
    t2d::log::info(
        "full snapshot tick={} tanks={} projectiles={}",
        snap.server_tick(),
        snap.tanks_size(),
        snap.projectiles_size());
    int shown = 0;
    for (const auto &t : snap.tanks()) {
        if (shown++ >= 3)
            break;
        t2d::log::info(
            " tank id={} pos=({:.2f},{:.2f}) hp={} ammo={} hull={:.1f} turret={:.1f}",
            t.entity_id(),
            t.x(),
            t.y(),
            t.hp(),
            t.ammo(),
            t.hull_angle(),
            t.turret_angle());
    }
}

static void log_delta_snapshot(const t2d::DeltaSnapshot &d)
{
    t2d::log::debug(
        "delta tick={} base={} dtanks={} dprojs={} removed_tanks={} removed_projs={}",
        d.server_tick(),
        d.base_tick(),
        d.tanks_size(),
        d.projectiles_size(),
        d.removed_tanks_size(),
        d.removed_projectiles_size());
    int shown = 0;
    for (const auto &t : d.tanks()) {
        if (shown++ >= 3)
            break;
        t2d::log::debug(
            " dtank id={} pos=({:.2f},{:.2f}) hp={} ammo={} hull={:.1f} turret={:.1f}",
            t.entity_id(),
            t.x(),
            t.y(),
            t.hp(),
            t.ammo(),
            t.hull_angle(),
            t.turret_angle());
    }
}

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

coro::task<bool> read_one(
    coro::net::tcp::client &client, t2d::ServerMessage &out, std::chrono::milliseconds /*time_left*/)
{
    static t2d::netutil::FrameParseState state;
    // time_left is currently unused; we perform a single non-blocking recv attempt.
    std::string tmp(2048, '\0');
    auto [st, span] = client.recv(tmp); // non-blocking attempt
    if (st == coro::net::recv_status::closed)
        co_return false;
    if (st == coro::net::recv_status::would_block)
        co_return false; // no data this iteration
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

// Coroutine entry; first await binds to scheduler thread per project coroutine policy.
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
    uint64_t loop_iter = 0; // still used for synthetic movement phase progression
    std::string session_id;
    uint32_t last_full_tick = 0;
    // Time-based scheduling state
    constexpr auto heartbeat_interval = std::chrono::milliseconds(1000);
    constexpr auto input_interval = std::chrono::milliseconds(100); // previously every 5 * 20ms
    std::chrono::milliseconds iteration_budget{20}; // will be updated after match_start using server tick_rate
    auto last_heartbeat = std::chrono::steady_clock::now();
    auto last_input = std::chrono::steady_clock::now();
    uint32_t client_tick_counter = 0;
    while (!g_shutdown.load()) {
        auto iter_start = std::chrono::steady_clock::now();
        // Heartbeat based on elapsed time
        if (iter_start - last_heartbeat >= heartbeat_interval) {
            last_heartbeat = iter_start;
            t2d::ClientMessage hb;
            auto *h = hb.mutable_heartbeat();
            h->set_session_id(session_id);
            h->set_time_ms((uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count());
            co_await send_frame(cli, hb);
        }
        // Input (movement + fire pulse) every input_interval while in a match
        if (in_match && (iter_start - last_input >= input_interval)) {
            last_input = iter_start;
            t2d::ClientMessage in;
            auto *ic = in.mutable_input();
            ic->set_session_id(session_id);
            ic->set_client_tick(client_tick_counter++);
            float phase = static_cast<float>(loop_iter % 360) * 3.14159f / 180.0f;
            ic->set_move_dir(std::sin(phase));
            ic->set_turn_dir(std::cos(phase));
            ic->set_turret_turn(std::sin(phase * 0.5f));
            // Fire every 30 input messages ~3s (matches old 150 * 20ms)
            ic->set_fire((client_tick_counter % 30) == 0);
            co_await send_frame(cli, in);
        }
        // Remaining time budget for this iteration passed to read_one
        auto after_sends = std::chrono::steady_clock::now();
        auto elapsed = after_sends - iter_start;
        auto time_left = (elapsed >= iteration_budget)
            ? std::chrono::milliseconds(0)
            : std::chrono::duration_cast<std::chrono::milliseconds>(iteration_budget - elapsed);
        t2d::ServerMessage sm;
        bool got = co_await read_one(cli, sm, time_left);
        if (!got && time_left.count() > 0) {
            // Sleep out the remaining budget to keep ~20ms cadence without busy spinning.
            co_await sched->yield_for(time_left);
        }
        if (got) {
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
                auto tick_rate = sm.match_start().tick_rate();
                if (tick_rate > 0 && tick_rate <= 1000) { // basic sanity
                    iteration_budget = std::chrono::milliseconds(1000 / tick_rate);
                } else {
                    iteration_budget = std::chrono::milliseconds(20); // fallback
                }
                t2d::log::info(
                    "match start id={} tick_rate={} seed={} iteration_budget_ms={}",
                    sm.match_start().match_id(),
                    tick_rate,
                    sm.match_start().seed(),
                    iteration_budget.count());
            } else if (sm.has_snapshot()) {
                last_full_tick = sm.snapshot().server_tick();
                log_full_snapshot(sm.snapshot());
            } else if (sm.has_delta_snapshot()) {
                log_delta_snapshot(sm.delta_snapshot());
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
        // No local world summary (raw snapshots already logged)
        ++loop_iter;
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
