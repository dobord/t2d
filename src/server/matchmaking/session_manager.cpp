// SPDX-License-Identifier: Apache-2.0
#include "server/matchmaking/session_manager.hpp"

#include "common/logger.hpp"
#include "common/metrics.hpp"

#include <algorithm>

namespace t2d::mm {

SessionManager &instance()
{
    static SessionManager inst;
    return inst;
}

std::shared_ptr<Session> SessionManager::add_connection(coro::net::tcp::client client)
{
    std::scoped_lock lk{m_mutex};
    std::string cid = "conn_" + std::to_string(++m_connection_counter);
    auto s = std::make_shared<Session>(cid, std::move(client));
    m_by_connection.emplace(cid, s);
    return s;
}

void SessionManager::authenticate(const std::shared_ptr<Session> &s, std::string session_id)
{
    std::scoped_lock lk{m_mutex};
    s->authenticated = true;
    s->session_id = std::move(session_id);
    s->last_heartbeat = std::chrono::steady_clock::now();
    m_by_session[s->session_id] = s;
    // metrics increment for connected authenticated players (excluding bots)
    if (!s->is_bot)
        t2d::metrics::runtime().connected_players.fetch_add(1, std::memory_order_relaxed);
}

void SessionManager::enqueue(const std::shared_ptr<Session> &s)
{
    std::scoped_lock lk{m_mutex};
    if (!s->in_queue) {
        s->in_queue = true;
        s->queue_join_time = std::chrono::steady_clock::now();
        m_queue.push_back(s);
    }
}

std::vector<std::shared_ptr<Session>> SessionManager::snapshot_queue()
{
    std::scoped_lock lk{m_mutex};
    return m_queue; // copy of vector (shared_ptr copied)
}

void SessionManager::pop_from_queue(const std::vector<std::shared_ptr<Session>> &sessions)
{
    std::scoped_lock lk{m_mutex};
    // remove these sessions
    m_queue.erase(
        std::remove_if(
            m_queue.begin(),
            m_queue.end(),
            [&](auto &sp) { return std::find(sessions.begin(), sessions.end(), sp) != sessions.end(); }),
        m_queue.end());
    for (auto &s : sessions) {
        s->in_queue = false;
    }
}

void SessionManager::push_message(const std::shared_ptr<Session> &s, const t2d::ServerMessage &msg)
{
    std::scoped_lock lk{m_mutex};
    if (s->is_bot)
        return; // bots do not receive network messages (prototype)
    s->outgoing.push_back(msg);
}

std::vector<t2d::ServerMessage> SessionManager::drain_messages(const std::shared_ptr<Session> &s)
{
    std::scoped_lock lk{m_mutex};
    std::vector<t2d::ServerMessage> out;
    out.swap(s->outgoing);
    return out;
}

void SessionManager::update_heartbeat(const std::shared_ptr<Session> &s)
{
    std::scoped_lock lk{m_mutex};
    s->last_heartbeat = std::chrono::steady_clock::now();
}

void SessionManager::update_input(const std::shared_ptr<Session> &s, const t2d::InputCommand &cmd)
{
    std::scoped_lock lk{m_mutex};
    if (cmd.client_tick() < s->input.last_client_tick)
        return; // ignore old
    bool move_changed = s->input.move_dir != cmd.move_dir();
    bool turn_changed = s->input.turn_dir != cmd.turn_dir();
    bool turret_changed = s->input.turret_turn != cmd.turret_turn();
    bool fire_changed = s->input.fire != cmd.fire();
    bool brake_changed = s->input.brake != cmd.brake();
    s->input.last_client_tick = cmd.client_tick();
    s->input.move_dir = cmd.move_dir();
    s->input.turn_dir = cmd.turn_dir();
    s->input.turret_turn = cmd.turret_turn();
    s->input.fire = cmd.fire();
    s->input.brake = cmd.brake();
    if (!s->is_bot && (move_changed || turn_changed || turret_changed || fire_changed || brake_changed)) {
        t2d::log::debug(
            "[input] session={} ctick={} move={} turn={} turret={} fire={} brake={}",
            s->session_id,
            s->input.last_client_tick,
            s->input.move_dir,
            s->input.turn_dir,
            s->input.turret_turn,
            s->input.fire,
            s->input.brake);
    }
}

Session::InputState SessionManager::get_input_copy(const std::shared_ptr<Session> &s)
{
    std::scoped_lock lk{m_mutex};
    return s->input;
}

std::vector<std::shared_ptr<Session>> SessionManager::snapshot_all_sessions()
{
    std::scoped_lock lk{m_mutex};
    std::vector<std::shared_ptr<Session>> res;
    res.reserve(m_by_session.size());
    for (auto &kv : m_by_session)
        res.push_back(kv.second);
    return res;
}

void SessionManager::disconnect_session(const std::shared_ptr<Session> &s)
{
    std::scoped_lock lk{m_mutex};
    if (s->in_queue) {
        m_queue.erase(std::remove(m_queue.begin(), m_queue.end(), s), m_queue.end());
        s->in_queue = false;
    }
    if (!s->session_id.empty())
        m_by_session.erase(s->session_id);
    m_by_connection.erase(s->connection_id);
    if (!s->is_bot && s->authenticated) {
        auto &cp = t2d::metrics::runtime().connected_players;
        auto cur = cp.load(std::memory_order_relaxed);
        if (cur > 0)
            cp.fetch_sub(1, std::memory_order_relaxed);
    }
}

std::vector<std::shared_ptr<Session>> SessionManager::create_bots(size_t count)
{
    std::scoped_lock lk{m_mutex};
    std::vector<std::shared_ptr<Session>> created;
    created.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        auto s = std::make_shared<Session>();
        s->is_bot = true;
        s->authenticated = true;
        s->session_id = "bot_" + std::to_string(++m_bot_counter);
        s->last_heartbeat = std::chrono::steady_clock::now();
        s->in_queue = true;
        s->queue_join_time = std::chrono::steady_clock::now();
        m_queue.push_back(s);
        m_by_session[s->session_id] = s;
        created.push_back(s);
    }
    return created;
}

void SessionManager::set_bot_input(const std::shared_ptr<Session> &s, const Session::InputState &st)
{
    std::scoped_lock lk{m_mutex};
    if (!s->is_bot)
        return;
    s->input = st;
}

void SessionManager::clear_bot_fire(const std::shared_ptr<Session> &s)
{
    std::scoped_lock lk{m_mutex};
    if (!s->is_bot)
        return;
    s->input.fire = false;
}

} // namespace t2d::mm
