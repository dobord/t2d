#include "common/framing.hpp"
#include "game.pb.h"

#include <arpa/inet.h>
#include <coro/coro.hpp>
#include <coro/io_scheduler.hpp>
#include <coro/net/tcp/client.hpp>

#include <iostream>
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

static coro::task<void> client_flow(std::shared_ptr<coro::io_scheduler> scheduler, uint16_t port)
{
    co_await scheduler->schedule();
    coro::net::tcp::client cli{scheduler, {.address = coro::net::ip_address::from_string("127.0.0.1"), .port = port}};
    auto cstatus = co_await cli.connect(5s);
    if (cstatus != coro::net::connect_status::connected) {
        std::cerr << "Connect failed\n";
        co_return;
    }
    std::cout << "Connected\n";
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
    // Wait for MatchStart
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < 10s) {
        t2d::ServerMessage sm;
        if (!co_await read_frame(cli, sm))
            continue;
        if (sm.has_auth_response()) {
            std::cout << "AuthResponse success=" << sm.auth_response().success() << std::endl;
        } else if (sm.has_queue_status()) {
            std::cout << "Queue position=" << sm.queue_status().position() << std::endl;
        } else if (sm.has_match_start()) {
            std::cout << "MatchStart id=" << sm.match_start().match_id() << " seed=" << sm.match_start().seed()
                      << std::endl;
            co_return;
        }
    }
    std::cout << "Timeout waiting match start" << std::endl;
}

int main(int argc, char **argv)
{
    uint16_t port = 40000;
    if (argc > 1)
        port = static_cast<uint16_t>(std::stoi(argv[1]));
    auto scheduler = coro::io_scheduler::make_shared();
    coro::sync_wait(client_flow(scheduler, port));
    return 0;
}
