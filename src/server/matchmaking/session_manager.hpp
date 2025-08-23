// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "game.pb.h"

#include <coro/net/tcp/client.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace t2d::mm {

struct Session : public std::enable_shared_from_this<Session>
{
    std::string connection_id; // internal id until auth (empty for bot)
    std::string session_id; // set after auth or bot id
    bool authenticated{false};
    bool in_queue{false};
    bool is_bot{false};
    // Match association (set when a match starts). Weak reference to avoid lifetime cycles.
    std::weak_ptr<void> match_ctx; // cast to t2d::game::MatchContext in implementation to avoid circular include
    uint32_t tank_entity_id{0}; // entity id inside the match (0 if not in a match)
    std::chrono::steady_clock::time_point queue_join_time{};
    std::chrono::steady_clock::time_point last_heartbeat{}; // updated on heartbeat
    // When a lobby countdown has started for the group this player is in (earliest join triggered)
    // Not persisted; recalculated by matchmaker each poll.
    std::chrono::steady_clock::time_point lobby_countdown_start{}; // 0 if not in countdown yet

    struct InputState
    {
        float move_dir{0.f};
        float turn_dir{0.f};
        float turret_turn{0.f};
        bool fire{false};
        uint32_t last_client_tick{0};
    } input;

    std::unique_ptr<coro::net::tcp::client> client; // nullptr for bots
    std::vector<t2d::ServerMessage> outgoing; // pending outbound messages

    Session(std::string cid, coro::net::tcp::client c)
        : connection_id(std::move(cid)), client(std::make_unique<coro::net::tcp::client>(std::move(c)))
    {}

    Session() = default; // bot constructor
};

class SessionManager
{
public:
    std::shared_ptr<Session> add_connection(coro::net::tcp::client client);
    void authenticate(const std::shared_ptr<Session> &s, std::string session_id);
    void enqueue(const std::shared_ptr<Session> &s);
    std::vector<std::shared_ptr<Session>> snapshot_queue();
    void pop_from_queue(const std::vector<std::shared_ptr<Session>> &sessions);
    void push_message(const std::shared_ptr<Session> &s, const t2d::ServerMessage &msg);
    std::vector<t2d::ServerMessage> drain_messages(const std::shared_ptr<Session> &s);
    void update_heartbeat(const std::shared_ptr<Session> &s);
    void update_input(const std::shared_ptr<Session> &s, const t2d::InputCommand &cmd);
    Session::InputState get_input_copy(const std::shared_ptr<Session> &s);
    std::vector<std::shared_ptr<Session>> snapshot_all_sessions();
    void disconnect_session(const std::shared_ptr<Session> &s);
    // Create and enqueue the given number of bot sessions; returns created bots
    std::vector<std::shared_ptr<Session>> create_bots(size_t count);
    // Directly set input for a bot (no client tick ordering)
    void set_bot_input(const std::shared_ptr<Session> &s, const Session::InputState &st);
    void clear_bot_fire(const std::shared_ptr<Session> &s);

private:
    std::mutex m_mutex;
    uint64_t m_connection_counter{0};
    uint64_t m_bot_counter{0};
    std::unordered_map<std::string, std::shared_ptr<Session>> m_by_connection; // pre-auth
    std::unordered_map<std::string, std::shared_ptr<Session>> m_by_session; // post-auth
    std::vector<std::shared_ptr<Session>> m_queue; // FIFO queue of players waiting matchmaking
};

// Global accessor (simple singleton for early prototype)
SessionManager &instance();

} // namespace t2d::mm
