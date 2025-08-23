// SPDX-License-Identifier: Apache-2.0
// e2e_bot_projectile.cpp
// Ensures that after bot fill, a projectile from a bot appears in snapshots.
#include "common/framing.hpp"
#include "game.pb.h"
#include "server/matchmaking/matchmaker.hpp"
#include "server/matchmaking/session_manager.hpp"
#include "server/net/listener.hpp"

#include <coro/coro.hpp>
#include <coro/default_executor.hpp>
#include <coro/io_scheduler.hpp>
#include <coro/net/tcp/client.hpp>

#include <cassert>
#include <iostream>
using namespace std::chrono_literals;

static coro::task<void> flow(std::shared_ptr<coro::io_scheduler> sched, uint16_t port)
{
    co_await sched->yield_for(50ms);
    coro::net::tcp::client cli{sched, {.address = coro::net::ip_address::from_string("127.0.0.1"), .port = port}};
    auto st = co_await cli.connect(2s);
    assert(st == coro::net::connect_status::connected);
    // Auth
    t2d::ClientMessage auth;
    auth.mutable_auth_request()->set_oauth_token("x");
    auth.mutable_auth_request()->set_client_version("t");
    std::string payload;
    auth.SerializeToString(&payload);
    auto fr = t2d::netutil::build_frame(payload);
    std::span<const char> rest(fr.data(), fr.size());
    while (!rest.empty()) {
        co_await cli.poll(coro::poll_op::write);
        auto [ss, r] = cli.send(rest);
        if (ss == coro::net::send_status::ok || ss == coro::net::send_status::would_block)
            rest = r;
        else
            co_return;
    }
    // Queue
    t2d::ClientMessage q;
    q.mutable_queue_join();
    q.SerializeToString(&payload);
    fr = t2d::netutil::build_frame(payload);
    rest = {fr.data(), fr.size()};
    while (!rest.empty()) {
        co_await cli.poll(coro::poll_op::write);
        auto [ss, r] = cli.send(rest);
        if (ss == coro::net::send_status::ok || ss == coro::net::send_status::would_block)
            rest = r;
        else
            co_return;
    }
    t2d::netutil::FrameParseState fps;
    bool gotMatch = false;
    bool sawProjectile = false;
    auto deadline = std::chrono::steady_clock::now() + 8s;
    while (std::chrono::steady_clock::now() < deadline && (!gotMatch || !sawProjectile)) {
        co_await cli.poll(coro::poll_op::read, 150ms);
        std::string tmp(2048, '\0');
        auto [rs, span] = cli.recv(tmp);
        if (rs == coro::net::recv_status::would_block)
            continue;
        if (rs == coro::net::recv_status::closed)
            break;
        if (rs != coro::net::recv_status::ok)
            break;
        fps.buffer.insert(fps.buffer.end(), span.begin(), span.end());
        std::string pl;
        while (t2d::netutil::try_extract(fps, pl)) {
            t2d::ServerMessage sm;
            sm.ParseFromArray(pl.data(), (int)pl.size());
            if (sm.has_match_start())
                gotMatch = true;
            else if (sm.has_snapshot()) {
                if (sm.snapshot().projectiles_size() > 0) {
                    sawProjectile = true;
                }
            } else if (sm.has_delta_snapshot()) {
                if (sm.delta_snapshot().projectiles_size() > 0) {
                    sawProjectile = true;
                }
            }
        }
    }
    assert(gotMatch && sawProjectile);
    std::cout << "e2e_bot_projectile OK" << std::endl;
    co_return;
}

int main()
{
    auto sched = coro::default_executor::io_executor();
    uint16_t port = 41040;
    sched->spawn(t2d::net::run_listener(sched, port));
    // quick fill: 4 players target, 1s timeout. Force snapshot + full snapshot every tick (interval=1)
    // so the projectile fired on tick 1 appears in a full snapshot (test only inspects full snapshots).
    sched->spawn(t2d::mm::run_matchmaker(sched, t2d::mm::MatchConfig{4, 1, 30, 200, 1, 1}));
    coro::sync_wait(flow(sched, port));
    return 0;
}
