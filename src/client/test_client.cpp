// SPDX-License-Identifier: Apache-2.0
#include "common/framing.hpp"
#include "common/logger.hpp"
#include "game.pb.h"

#include <arpa/inet.h>
#include <coro/coro.hpp>
#include <coro/default_executor.hpp>
#include <coro/io_scheduler.hpp>
#include <coro/net/tcp/client.hpp>

#include <iostream>
#include <random>
#include <string>

using namespace std::chrono_literals;

static coro::task<void> send_frame(coro::net::tcp::client &client, const t2d::ClientMessage &msg)
{
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
        } else
            co_return;
    }
}

static coro::task<bool> read_frame(coro::net::tcp::client &client, t2d::ServerMessage &out)
{
    static t2d::netutil::FrameParseState state; // simple static for prototype
    // read available chunk
    co_await client.poll(coro::poll_op::read);
    std::string tmp(1024, '\0');
    auto [st, span] = client.recv(tmp);
    if (st == coro::net::recv_status::closed)
        co_return false;
    if (st == coro::net::recv_status::would_block)
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

static coro::task<void> client_flow(std::shared_ptr<coro::io_scheduler> scheduler, uint16_t port, uint32_t active_secs)
{
    co_await scheduler->schedule();
    coro::net::tcp::client cli{scheduler, {.address = coro::net::ip_address::from_string("127.0.0.1"), .port = port}};
    auto cstatus = co_await cli.connect(5s);
    if (cstatus != coro::net::connect_status::connected) {
        t2d::log::error("client connect failed");
        co_return;
    }
    t2d::log::info("client connected");
    std::string session_id;
    // Auth
    t2d::ClientMessage authMsg;
    auto *ar = authMsg.mutable_auth_request();
    ar->set_oauth_token("dummy");
    ar->set_client_version("dev");
    co_await send_frame(cli, authMsg);
    // Queue join
    t2d::ClientMessage q;
    q.mutable_queue_join();
    co_await send_frame(cli, q);
    // Phase 1: wait for AuthResponse + MatchStart
    auto wait_start = std::chrono::steady_clock::now();
    bool match_started = false;
    while (std::chrono::steady_clock::now() - wait_start < 15s && !match_started) {
        t2d::ServerMessage sm;
        if (!co_await read_frame(cli, sm))
            continue;
        if (sm.has_auth_response()) {
            session_id = sm.auth_response().session_id();
            t2d::log::info("AuthResponse success={} sid={}", sm.auth_response().success(), session_id);
        } else if (sm.has_queue_status()) {
            t2d::log::debug("Queue position={}", sm.queue_status().position());
        } else if (sm.has_match_start()) {
            t2d::log::info("MatchStart id={} seed={}", sm.match_start().match_id(), sm.match_start().seed());
            match_started = true;
        }
    }
    if (!match_started) {
        t2d::log::warn("Timeout waiting match start");
        co_return;
    }
    if (session_id.empty()) {
        t2d::log::warn("No session id captured; aborting active phase");
        co_return;
    }
    // Active phase: send inputs + heartbeat; simplistic random walk
    std::mt19937 rng{std::random_device{}()};
    std::uniform_real_distribution<float> dir(-1.f, 1.f);
    uint64_t client_tick = 0;
    auto active_start = std::chrono::steady_clock::now();
    auto next_hb = active_start;
    while (std::chrono::steady_clock::now() - active_start < std::chrono::seconds(active_secs)) {
        t2d::ClientMessage in;
        auto *ic = in.mutable_input();
        ic->set_session_id(session_id);
        ic->set_client_tick(static_cast<uint32_t>(client_tick++));
        ic->set_move_dir(dir(rng));
        ic->set_turn_dir(dir(rng) * 0.5f);
        ic->set_turret_turn(dir(rng));
        ic->set_fire((client_tick % 15) == 0); // periodic fire attempt
        ic->set_brake(false);
        co_await send_frame(cli, in);
        auto now = std::chrono::steady_clock::now();
        if (now >= next_hb) {
            t2d::ClientMessage hb;
            auto *h = hb.mutable_heartbeat();
            h->set_session_id(session_id);
            h->set_time_ms((uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(now - active_start).count());
            co_await send_frame(cli, hb);
            next_hb = now + 2s;
        }
        // Opportunistically read any server frames (non-strict; best effort)
        for (int i = 0; i < 2; ++i) {
            t2d::ServerMessage sm;
            if (!co_await read_frame(cli, sm))
                break; // nothing ready
        }
        co_await scheduler->yield_for(100ms);
    }
    t2d::log::info("Active phase complete (secs={})", active_secs);
}

int main(int argc, char **argv)
{
    uint16_t port = 40000;
    uint32_t active_secs = 20; // default active phase duration after match start
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--active-seconds" && i + 1 < argc) {
            active_secs = static_cast<uint32_t>(std::stoul(argv[++i]));
        } else if (!a.empty() && a[0] != '-') {
            // positional first non-flag is port (retain old CLI compatibility)
            port = static_cast<uint16_t>(std::stoi(a));
        }
    }
    auto env_active = std::getenv("T2D_ACTIVE_SECS");
    if (env_active) {
        try {
            active_secs = static_cast<uint32_t>(std::stoul(env_active));
        } catch (...) {
            t2d::log::warn("Invalid T2D_ACTIVE_SECS env value");
        }
    }
    auto scheduler = coro::default_executor::io_executor();
    coro::sync_wait(client_flow(scheduler, port, active_secs));
    return 0;
}
