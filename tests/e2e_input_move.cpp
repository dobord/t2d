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
    co_await sched->yield_for(50ms);
    coro::net::tcp::client cli{sched, {.address = coro::net::ip_address::from_string("127.0.0.1"), .port = port}};
    auto st = co_await cli.connect(2s);
    assert(st == coro::net::connect_status::connected);
    // auth
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
    // queue
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
    // wait match & first snapshot baseline
    t2d::netutil::FrameParseState fps;
    bool gotMatch = false;
    float startX = 0.f, startY = 0.f;
    bool haveBaseline = false;
    auto deadline = std::chrono::steady_clock::now() + 6s;
    while (std::chrono::steady_clock::now() < deadline && !haveBaseline) {
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
            if (sm.has_match_start())
                gotMatch = true;
            else if (sm.has_snapshot() && gotMatch) {
                if (sm.snapshot().tanks_size() > 0) {
                    startX = sm.snapshot().tanks(0).x();
                    startY = sm.snapshot().tanks(0).y();
                    haveBaseline = true;
                    break;
                }
            }
        }
    }
    assert(gotMatch && haveBaseline);
    // send forward move input
    t2d::ClientMessage in;
    auto *ic = in.mutable_input();
    ic->set_session_id("sess_t");
    ic->set_client_tick(1);
    ic->set_move_dir(1.0f);
    ic->set_turn_dir(0);
    ic->set_turret_turn(0);
    ic->set_fire(false);
    in.SerializeToString(&payload);
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
    // wait snapshot showing movement
    bool moved = false;
    deadline = std::chrono::steady_clock::now() + 6s;
    while (std::chrono::steady_clock::now() < deadline && !moved) {
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
            if (sm.has_snapshot() && sm.snapshot().tanks_size() > 0) {
                float nx = sm.snapshot().tanks(0).x();
                float ny = sm.snapshot().tanks(0).y();
                if (nx != startX || ny != startY) {
                    moved = true;
                    break;
                }
            }
        }
    }
    assert(moved);
    std::cout << "e2e_input_move OK" << std::endl;
    co_return;
}

int main()
{
    auto sched = coro::default_executor::io_executor();
    uint16_t port = 41010;
    sched->spawn(t2d::net::run_listener(sched, port));
    sched->spawn(t2d::mm::run_matchmaker(sched, t2d::mm::MatchConfig{1, 180, 30}));
    coro::sync_wait(client_flow(sched, port));
    return 0;
}
