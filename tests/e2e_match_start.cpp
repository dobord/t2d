// SPDX-License-Identifier: Apache-2.0
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

static coro::task<void> client_flow(std::shared_ptr<coro::io_scheduler> sched, uint16_t port)
{
    // First await: yield briefly to allow listener to bind the port before connecting.
    co_await sched->yield_for(100ms);
    coro::net::tcp::client cli{sched, {.address = coro::net::ip_address::from_string("127.0.0.1"), .port = port}};
    auto st = co_await cli.connect(2s);
    assert(st == coro::net::connect_status::connected);
    co_await sched->yield_for(50ms);
    // send auth
    t2d::ClientMessage auth;
    auth.mutable_auth_request()->set_oauth_token("x");
    auth.mutable_auth_request()->set_client_version("t");
    std::string payload;
    auth.SerializeToString(&payload);
    auto frame = t2d::netutil::build_frame(payload);
    std::span<const char> rest(frame.data(), frame.size());
    while (!rest.empty()) {
        co_await cli.poll(coro::poll_op::write);
        auto [s, r] = cli.send(rest);
        if (s == coro::net::send_status::ok || s == coro::net::send_status::would_block)
            rest = r;
        else
            co_return;
    }
    // queue join
    t2d::ClientMessage q;
    q.mutable_queue_join();
    q.SerializeToString(&payload);
    frame = t2d::netutil::build_frame(payload);
    rest = {frame.data(), frame.size()};
    while (!rest.empty()) {
        co_await cli.poll(coro::poll_op::write);
        auto [s, r] = cli.send(rest);
        if (s == coro::net::send_status::ok || s == coro::net::send_status::would_block)
            rest = r;
        else
            co_return;
    }
    // read frames until match start
    t2d::netutil::FrameParseState fps;
    bool gotAuth = false, gotQueue = false, gotMatch = false, gotSnapshot = false;
    auto deadline = std::chrono::steady_clock::now() + 8s; // extended for slower CI environments
    while (std::chrono::steady_clock::now() < deadline && (!gotMatch || !gotSnapshot)) {
        co_await cli.poll(coro::poll_op::read, 100ms);
        std::string chunk(1024, '\0');
        auto [rs, span] = cli.recv(chunk);
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
            bool parsed = sm.ParseFromArray(pl.data(), (int)pl.size());
            assert(parsed);
            if (sm.has_auth_response()) {
                gotAuth = true;
                std::cout << "[e2e] got AuthResponse" << std::endl;
            } else if (sm.has_queue_status()) {
                gotQueue = true;
                std::cout << "[e2e] got QueueStatus" << std::endl;
            } else if (sm.has_match_start()) {
                gotMatch = true;
                std::cout << "[e2e] got MatchStart" << std::endl;
            } else if (sm.has_snapshot()) {
                gotSnapshot = true;
                std::cout << "[e2e] got Snapshot tick=" << sm.snapshot().server_tick() << std::endl;
            } else {
                std::cout << "[e2e] got other server msg" << std::endl;
            }
        }
    }
    assert(gotAuth);
    assert(gotQueue);
    assert(gotMatch);
    assert(gotSnapshot);
    std::cout << "e2e_match_start OK" << std::endl;
    co_return;
}

int main()
{
    auto sched = coro::default_executor::io_executor();
    uint16_t port = 41000;
    sched->spawn(t2d::net::run_listener(sched, port));
    sched->spawn(t2d::mm::run_matchmaker(sched, t2d::mm::MatchConfig{1, 180, 30}));
    coro::sync_wait(client_flow(sched, port));
    return 0;
}
