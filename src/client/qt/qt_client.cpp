// SPDX-License-Identifier: Apache-2.0
#include "common/framing.hpp"
#include "common/logger.hpp"
#include "entity_model.hpp"
#include "game.pb.h"
#include "input_state.hpp"
#include "projectile_model.hpp"
#include "timing_state.hpp"

#include <coro/default_executor.hpp>
#include <coro/io_scheduler.hpp>
#include <coro/net/tcp/client.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <thread>

#include <QtGui/QGuiApplication>
#include <QtQml/QQmlApplicationEngine>
#include <QtQml/QQmlContext>

using namespace std::chrono_literals;

namespace {
std::atomic_bool g_shutdown{false};

void handle_sig(int)
{
    g_shutdown.store(true);
}

coro::task<void> send_frame(coro::net::tcp::client &client, const t2d::ClientMessage &msg)
{
    co_await client.poll(coro::poll_op::write);
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
        } else {
            co_return;
        }
    }
}

coro::task<bool> read_one(coro::net::tcp::client &client, t2d::ServerMessage &out)
{
    static t2d::netutil::FrameParseState state;
    co_await client.poll(coro::poll_op::read);
    std::string tmp(4096, '\0');
    auto [st, span] = client.recv(tmp);
    if (st == coro::net::recv_status::closed)
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

coro::task<void> run_network(
    std::shared_ptr<coro::io_scheduler> sched,
    EntityModel *tankModel,
    ProjectileModel *projModel,
    InputState *input,
    TimingState *timing,
    std::string host,
    uint16_t port)
{
    co_await sched->schedule();
    coro::net::tcp::client cli{sched, {.address = coro::net::ip_address::from_string(host), .port = port}};
    auto rc = co_await cli.connect(5s);
    if (rc != coro::net::connect_status::connected) {
        t2d::log::error("qt_client connect failed");
        co_return;
    }
    t2d::log::info("qt_client connected host={} port={}", host, port);
    t2d::ClientMessage auth;
    auto *rq = auth.mutable_auth_request();
    rq->set_oauth_token("qt_ui_dummy");
    rq->set_client_version(T2D_VERSION);
    co_await send_frame(cli, auth);
    t2d::ClientMessage q;
    q.mutable_queue_join();
    co_await send_frame(cli, q);
    std::string session_id;
    bool in_match = false;
    uint64_t loop_iter = 0;
    while (!g_shutdown.load()) {
        if ((loop_iter % 50) == 0) {
            t2d::ClientMessage hb;
            auto *h = hb.mutable_heartbeat();
            h->set_session_id(session_id);
            h->set_time_ms((uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count());
            co_await send_frame(cli, hb);
        }
        if (in_match && (loop_iter % 5) == 0) {
            t2d::ClientMessage in;
            auto *ic = in.mutable_input();
            ic->set_session_id(session_id);
            ic->set_client_tick((uint32_t)loop_iter);
            ic->set_move_dir(input->move());
            ic->set_turn_dir(input->turn());
            ic->set_turret_turn(input->turretTurn());
            ic->set_fire(input->fire());
            co_await send_frame(cli, in);
        }
        t2d::ServerMessage sm;
        if (co_await read_one(cli, sm)) {
            if (sm.has_auth_response()) {
                session_id = sm.auth_response().session_id();
            } else if (sm.has_match_start()) {
                in_match = true;
            } else if (sm.has_snapshot()) {
                tankModel->applyFull(sm.snapshot());
                projModel->applyFull(sm.snapshot());
                timing->markServerTick();
            } else if (sm.has_delta_snapshot()) {
                tankModel->applyDelta(sm.delta_snapshot());
                projModel->applyDelta(sm.delta_snapshot());
                timing->markServerTick();
            }
        }
        ++loop_iter;
        co_await sched->yield_for(20ms);
    }
    t2d::log::info("qt_client network loop exit");
}
} // namespace

int main(int argc, char **argv)
{
    std::signal(SIGINT, handle_sig);
    std::signal(SIGTERM, handle_sig);
    setenv("T2D_LOG_LEVEL", "info", 1);
    t2d::log::init();
    QGuiApplication app(argc, argv);
    EntityModel tankModel; // tanks
    ProjectileModel projectileModel; // projectiles
    InputState input;
    TimingState timing;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("entityModel", &tankModel);
    engine.rootContext()->setContextProperty("projectileModel", &projectileModel);
    engine.rootContext()->setContextProperty("inputState", &input);
    engine.rootContext()->setContextProperty("timingState", &timing);
    engine.load(QUrl("qrc:/T2DClient/Main.qml"));
    if (engine.rootObjects().isEmpty()) {
        t2d::log::error("Failed to load QML scene");
        return 1;
    }
    auto sched = coro::default_executor::io_executor();
    sched->spawn(run_network(sched, &tankModel, &projectileModel, &input, &timing, "127.0.0.1", 40000));
    int rc = app.exec();
    g_shutdown.store(true);
    return rc;
}
