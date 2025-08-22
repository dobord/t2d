#include "server/net/listener.hpp"

#include "common/framing.hpp"
#include "game.pb.h"
#include "server/matchmaking/session_manager.hpp"

#include <arpa/inet.h>
#include <coro/coro.hpp>
#include <coro/net/tcp/client.hpp>
#include <coro/net/tcp/server.hpp>
#include <coro/poll.hpp>

#include <iostream>
#include <string>
#include <vector>

namespace t2d::net {

// Forward declarations of per-connection coroutine.
static coro::task<void> connection_loop(
    std::shared_ptr<coro::io_scheduler> scheduler, std::shared_ptr<t2d::mm::Session> session);

coro::task<void> run_listener(std::shared_ptr<coro::io_scheduler> scheduler, uint16_t port)
{
    co_await scheduler->schedule();
    std::cout << "[listener] Starting TCP listener on port " << port << std::endl;
    coro::net::tcp::server server{scheduler, coro::net::tcp::server::options{.port = port}};
    while (true) {
        auto status = co_await server.poll();
        if (status == coro::poll_status::event) {
            auto client = server.accept();
            if (client.socket().is_valid()) {
                auto session = t2d::mm::instance().add_connection(std::move(client));
                scheduler->spawn(connection_loop(scheduler, session));
            }
        } else if (status == coro::poll_status::error || status == coro::poll_status::closed) {
            std::cerr << "[listener] Poll error/closed, exiting listener loop\n";
            co_return;
        }
    }
}

// Helper: read exactly n bytes into buffer (append), returns false on closed/error.
// Removed read_exact; replaced by streaming parser approach below.

// Helper: send all bytes of buffer.
static coro::task<void> send_all(coro::net::tcp::client &client, std::span<const char> data)
{
    std::span<const char> rest = data;
    while (!rest.empty()) {
        co_await client.poll(coro::poll_op::write);
        auto [s, remaining] = client.send(rest);
        if (s == coro::net::send_status::ok || s == coro::net::send_status::would_block) {
            rest = remaining;
            continue;
        } else {
            co_return; // abort on other errors
        }
    }
}

static coro::task<void> connection_loop(
    std::shared_ptr<coro::io_scheduler> scheduler, std::shared_ptr<t2d::mm::Session> session)
{
    co_await scheduler->schedule();
    std::cout << "[conn] New connection" << std::endl;
    t2d::netutil::FrameParseState fps; // streaming frame parser state
    while (true) {
        // Flush pending outbound first (if any)
        auto pending = t2d::mm::instance().drain_messages(session);
        if (!pending.empty()) {
            std::string batch;
            batch.reserve(pending.size() * 64); // heuristic
            for (auto &msg : pending) {
                std::string out;
                if (!msg.SerializeToString(&out))
                    continue;
                uint32_t out_len = htonl(static_cast<uint32_t>(out.size()));
                size_t offset = batch.size();
                batch.resize(offset + 4 + out.size());
                std::memcpy(batch.data() + offset, &out_len, 4);
                std::memcpy(batch.data() + offset + 4, out.data(), out.size());
            }
            if (session->client)
                co_await send_all(*session->client, std::span<const char>(batch.data(), batch.size()));
        }
        // Poll read with small timeout so loop progresses to flush snapshots
        if (!session->client)
            co_return; // bot session should never be here
        auto pstat = co_await session->client->poll(coro::poll_op::read, std::chrono::milliseconds(50));
        if (pstat == coro::poll_status::timeout) {
            continue;
        }
        // Read available chunk
        std::string tmp(1024, '\0');
        auto [rstatus, span] = session->client->recv(tmp);
        if (rstatus == coro::net::recv_status::closed) {
            std::cout << "[conn] Closed by peer" << std::endl;
            co_return;
        }
        if (rstatus != coro::net::recv_status::ok && rstatus != coro::net::recv_status::would_block) {
            std::cerr << "[conn] recv error" << std::endl;
            co_return;
        }
        if (rstatus == coro::net::recv_status::ok) {
            fps.buffer.insert(fps.buffer.end(), span.begin(), span.end());
        }
        std::string payload;
        while (t2d::netutil::try_extract(fps, payload)) {
            t2d::ClientMessage cmsg;
            if (!cmsg.ParseFromArray(payload.data(), (int)payload.size())) {
                std::cerr << "[conn] Failed to parse protobuf, dropping connection" << std::endl;
                co_return;
            }
            t2d::ServerMessage smsg;
            if (cmsg.has_auth_request()) {
                const auto &ar = cmsg.auth_request();
                auto *resp = smsg.mutable_auth_response();
                resp->set_success(true);
                std::string sid = "sess_" + ar.client_version();
                resp->set_session_id(sid);
                resp->set_reason("");
                t2d::mm::instance().authenticate(session, sid);
                std::cout << "[conn] AuthRequest -> success sid=" << sid << std::endl;
            } else if (cmsg.has_queue_join()) {
                auto *qs = smsg.mutable_queue_status();
                qs->set_position(1);
                qs->set_players_in_queue(1);
                qs->set_needed_for_match(16);
                qs->set_timeout_seconds_left(180);
                if (session->authenticated) {
                    t2d::mm::instance().enqueue(session);
                }
                std::cout << "[conn] QueueJoin received (enqueued=" << (session->authenticated ? "yes" : "no-auth")
                          << ")" << std::endl;
            } else if (cmsg.has_heartbeat()) {
                t2d::mm::instance().update_heartbeat(session);
                auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now().time_since_epoch())
                                  .count();
                t2d::ServerMessage hb;
                auto *hbr = hb.mutable_heartbeat_resp();
                hbr->set_session_id(session->session_id);
                hbr->set_client_time_ms(cmsg.heartbeat().time_ms());
                hbr->set_server_time_ms(now_ms);
                auto client_ms = static_cast<int64_t>(cmsg.heartbeat().time_ms());
                int64_t diff = static_cast<int64_t>(now_ms) - client_ms;
                if (diff < 0)
                    diff = 0;
                hbr->set_delta_ms(static_cast<uint64_t>(diff));
                t2d::mm::instance().push_message(session, hb);
                continue;
            } else if (cmsg.has_input()) {
                if (session->authenticated) {
                    t2d::mm::instance().update_input(session, cmsg.input());
                }
                continue; // no immediate ack
            } else {
                continue; // ignore others
            }
            std::string out;
            if (!smsg.SerializeToString(&out)) {
                std::cerr << "[conn] Failed serialize server msg" << std::endl;
                continue;
            }
            uint32_t out_len = htonl(static_cast<uint32_t>(out.size()));
            std::string frame;
            frame.resize(4 + out.size());
            std::memcpy(frame.data(), &out_len, 4);
            std::memcpy(frame.data() + 4, out.data(), out.size());
            if (session->client)
                co_await send_all(*session->client, std::span<const char>(frame.data(), frame.size()));
            std::cout << "[conn] Sent server message type="
                      << (smsg.has_auth_response()      ? "AuthResponse"
                              : smsg.has_queue_status() ? "QueueStatus"
                              : smsg.has_match_start()  ? "MatchStart"
                                                        : "Other")
                      << std::endl;
        }
    }
}

} // namespace t2d::net
