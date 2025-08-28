// SPDX-License-Identifier: Apache-2.0
// e2e_bot_fill.cpp
// Verifies that a single real player is matched with bots after fill timeout.
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

static coro::task<void> client_flow(std::shared_ptr<coro::io_scheduler> sched, uint16_t port)
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
    auto f = t2d::netutil::build_frame(payload);
    std::span<const char> rest(f.data(), f.size());
    while (!rest.empty()) {
        co_await cli.poll(coro::poll_op::write);
        auto [ss, r] = cli.send(rest);
        if (ss == coro::net::send_status::ok || ss == coro::net::send_status::would_block)
            rest = r;
        else
            co_return;
    }
    // Queue join
    t2d::ClientMessage q;
    q.mutable_queue_join();
    q.SerializeToString(&payload);
    f = t2d::netutil::build_frame(payload);
    rest = {f.data(), f.size()};
    while (!rest.empty()) {
        co_await cli.poll(coro::poll_op::write);
        auto [ss, r] = cli.send(rest);
        if (ss == coro::net::send_status::ok || ss == coro::net::send_status::would_block)
            rest = r;
        else
            co_return;
    }
    // Wait for match start & baseline snapshot (with bots filling). Use generous deadline.
    t2d::netutil::FrameParseState fps;
    bool gotMatch = false, gotSnapshot = false;
    auto deadline = std::chrono::steady_clock::now() + 6s;
    while (std::chrono::steady_clock::now() < deadline && (!gotMatch || !gotSnapshot)) {
        co_await cli.poll(coro::poll_op::read, 100ms);
        std::string tmp(1024, '\0');
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
            if (sm.has_match_start()) {
                gotMatch = true;
            } else if (sm.has_snapshot()) {
                gotSnapshot = true;
            }
        }
    }
    assert(gotMatch && gotSnapshot);
    std::cout << "e2e_bot_fill OK" << std::endl;
    co_return;
}

int main(int argc, char **argv)
{
    auto sched = coro::default_executor::io_executor();
    uint16_t port = 41030;
    t2d::mm::MatchConfig mc{4, 1, 30};
    if (argc > 1) {
        t2d::test::apply_match_config_overrides(mc, argv[1]);
    }
    const uint32_t tickRate = 60;
    sched->spawn(t2d::net::run_listener(sched, port, tickRate));
    sched->spawn(t2d::mm::run_matchmaker(sched, mc));
    coro::sync_wait(client_flow(sched, port));
    return 0;
}
