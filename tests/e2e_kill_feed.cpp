// SPDX-License-Identifier: Apache-2.0
// e2e_kill_feed.cpp
// Validates that when a lethal hit occurs, a KillFeedUpdate is eventually received.
#include "common/framing.hpp"
#include "game.pb.h"
#include "server/matchmaking/matchmaker.hpp"
#include "server/matchmaking/session_manager.hpp"
#include "server/net/listener.hpp"
#include "test_match_config_loader.hpp"

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
    bool gotKillFeed = false;
    bool gotDestroyed = false;
    auto deadline = std::chrono::steady_clock::now() + 20s;
    while (std::chrono::steady_clock::now() < deadline && (!gotMatch || !gotKillFeed)) {
        co_await cli.poll(coro::poll_op::read, 200ms);
        std::string tmp(4096, '\0');
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
            else if (sm.has_kill_feed())
                gotKillFeed = sm.kill_feed().events_size() > 0;
            else if (sm.has_destroyed())
                gotDestroyed = true; // auxiliary sanity
        }
    }
    assert(gotMatch);
    assert(gotDestroyed); // ensure lethal event occurred
    assert(gotKillFeed); // kill feed aggregated
    std::cout << "e2e_kill_feed OK" << std::endl;
    co_return;
}

int main(int argc, char **argv)
{
    auto sched = coro::default_executor::io_executor();
    uint16_t port = 41062;
    t2d::mm::MatchConfig mc{2, 1, 30};
    if (argc > 1) {
        t2d::test::apply_match_config_overrides(mc, argv[1]);
    }
    const uint32_t tickRate = 60;
    sched->spawn(t2d::net::run_listener(sched, port, tickRate));
    // quick bot fill (file can override)
    sched->spawn(t2d::mm::run_matchmaker(sched, mc));
    coro::sync_wait(flow(sched, port));
    return 0;
}
