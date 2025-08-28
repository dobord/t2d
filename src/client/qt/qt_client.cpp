// SPDX-License-Identifier: Apache-2.0
#include "ammo_box_model.hpp"
#include "common/framing.hpp"
#include "common/logger.hpp"
#include "crate_model.hpp"
#include "entity_model.hpp"
#include "game.pb.h"
#include "input_state.hpp"
#include "lobby_state.hpp"
#include "projectile_model.hpp"
#include "timing_state.hpp"

#include <coro/default_executor.hpp>
#include <coro/io_scheduler.hpp>
#include <coro/net/tcp/client.hpp>
#include <unistd.h> // getpid for oauth token suffix

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring> // std::strncmp
#include <iomanip>
#include <sstream>
#include <thread>

#include <QtGui/QGuiApplication>
#include <QtQml/QQmlApplicationEngine>
#include <QtQml/QQmlContext>
#include <QtQuick/QQuickWindow>

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

// Attempts to extract one message within the provided time budget; returns
//  1 = message parsed
//  0 = need more data (no message yet)
// -1 = connection closed or fatal parse
coro::task<int> read_one(coro::net::tcp::client &client, t2d::ServerMessage &out, std::chrono::milliseconds time_left)
{
    static t2d::netutil::FrameParseState state; // per-process (single connection prototype)
    // Try extract without new read first
    {
        std::string payload;
        if (t2d::netutil::try_extract(state, payload)) {
            if (out.ParseFromArray(payload.data(), (int)payload.size()))
                co_return 1;
            co_return -1;
        }
    }
    if (time_left.count() <= 0) {
        co_return 0; // no budget to poll
    }
    // Poll respecting remaining budget (cap a little so extremely long budgets do not block loop).
    auto poll_budget = (time_left > std::chrono::milliseconds(10)) ? std::chrono::milliseconds(10) : time_left;
    auto pstat = co_await client.poll(coro::poll_op::read, poll_budget);
    if (pstat == coro::poll_status::timeout) {
        co_return 0; // no data ready right now
    }
    if (pstat == coro::poll_status::error || pstat == coro::poll_status::closed) {
        // Attempt final extraction before declaring closed
        std::string payload;
        if (t2d::netutil::try_extract(state, payload) && out.ParseFromArray(payload.data(), (int)payload.size()))
            co_return 1;
        co_return -1;
    }
    std::string tmp(4096, '\0');
    auto [st, span] = client.recv(tmp);
    if (st == coro::net::recv_status::closed) {
        std::string payload;
        if (t2d::netutil::try_extract(state, payload) && out.ParseFromArray(payload.data(), (int)payload.size()))
            co_return 1;
        co_return -1;
    }
    if (st != coro::net::recv_status::ok)
        co_return 0; // would_block or other transient
    state.buffer.insert(state.buffer.end(), span.begin(), span.end());
    std::string payload;
    if (!t2d::netutil::try_extract(state, payload))
        co_return 0;
    if (!out.ParseFromArray(payload.data(), (int)payload.size()))
        co_return -1;
    co_return 1;
}

coro::task<void> run_network(
    std::shared_ptr<coro::io_scheduler> sched,
    EntityModel *tankModel,
    ProjectileModel *projModel,
    AmmoBoxModel *ammoModel,
    CrateModel *crateModel,
    InputState *input,
    TimingState *timing,
    LobbyState *lobby,
    std::string host,
    uint16_t port,
    const std::string &oauth_token)
{
    co_await sched->schedule();
    coro::net::tcp::client cli{sched, {.address = coro::net::ip_address::from_string(host), .port = port}};
    auto rc = co_await cli.connect(5s);
    if (rc != coro::net::connect_status::connected) {
        t2d::log::error("qt_client connect failed status={} host={} port={}", (int)rc, host, port);
        co_return;
    }
    t2d::log::info("qt_client connected host={} port={} status=connected", host, port);
    t2d::ClientMessage auth;
    auto *rq = auth.mutable_auth_request();
    rq->set_oauth_token(oauth_token);
    rq->set_client_version(T2D_VERSION);
    co_await send_frame(cli, auth);
    t2d::log::debug("auth_request sent token_len={}", oauth_token.size());
    t2d::ClientMessage q;
    q.mutable_queue_join();
    co_await send_frame(cli, q);
    t2d::log::debug("queue_join sent");
    std::string session_id;
    bool in_match = false;
    uint32_t myEntityId = 0; // authoritative from MatchStart
    // lobbyState now tracked via LobbyState object in QML
    uint64_t tickRate = 20; // default (Hz)
    uint64_t hardCapTicks = 0;
    uint64_t matchStartServerTick = 0;
    uint64_t loop_iter = 0; // still used for diagnostics
    // Time-based scheduling (mirrors desktop client refactor)
    constexpr auto heartbeat_interval = std::chrono::milliseconds(1000);
    std::chrono::milliseconds iteration_budget{20}; // will be updated after match_start according to tickRate
    std::chrono::milliseconds input_interval = iteration_budget / 2; // input at twice server tick rate by default
    auto last_heartbeat = std::chrono::steady_clock::now();
    auto last_input = std::chrono::steady_clock::now();
    uint32_t client_tick_counter = 0;
    // Profiling (enable via env T2D_PROFILE=1). Aggregates over 5 second windows.
    const bool profiling_enabled = (std::getenv("T2D_PROFILE") != nullptr);

    struct ProfAgg
    {
        std::chrono::steady_clock::time_point window_start{};
        uint64_t loops = 0;
        uint64_t msgs = 0;
        uint64_t heartbeats = 0;
        uint64_t inputs = 0;
        double loop_time_acc_ms = 0.0;
    } prof;

    if (profiling_enabled) {
        prof.window_start = std::chrono::steady_clock::now();
    }
    while (!g_shutdown.load()) {
        auto iter_start = std::chrono::steady_clock::now();
        // Heartbeat by elapsed time
        if (iter_start - last_heartbeat >= heartbeat_interval) {
            last_heartbeat = iter_start;
            t2d::ClientMessage hb;
            auto *h = hb.mutable_heartbeat();
            h->set_session_id(session_id);
            h->set_time_ms((uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count());
            co_await send_frame(cli, hb);
            if (profiling_enabled)
                ++prof.heartbeats;
            t2d::log::debug(
                "heartbeat sent in_match={} myEntityId={} ctick={}", in_match, myEntityId, client_tick_counter);
        }
        // Input send based on elapsed time
        if (in_match && (iter_start - last_input >= input_interval)) {
            last_input = iter_start;
            t2d::ClientMessage in;
            auto *ic = in.mutable_input();
            ic->set_session_id(session_id);
            ic->set_client_tick(client_tick_counter++);
            ic->set_move_dir(input->move());
            ic->set_turn_dir(input->turn());
            ic->set_turret_turn(input->turretTurn());
            ic->set_fire(input->fire());
            ic->set_brake(input->brake());
            t2d::log::debug(
                "send_input ctick={} move={} turn={} turret={} fire={} brake={}",
                client_tick_counter - 1,
                input->move(),
                input->turn(),
                input->turretTurn(),
                input->fire(),
                input->brake());
            co_await send_frame(cli, in);
            if (profiling_enabled)
                ++prof.inputs;
        }
        // Remaining time budget after outbound sends
        auto after_sends = std::chrono::steady_clock::now();
        auto elapsed = after_sends - iter_start;
        auto time_left = (elapsed >= iteration_budget)
            ? std::chrono::milliseconds(0)
            : std::chrono::duration_cast<std::chrono::milliseconds>(iteration_budget - elapsed);
        // Single receive attempt bounded by remaining budget
        if (time_left.count() > 0) {
            t2d::ServerMessage sm;
            int r = co_await read_one(cli, sm, time_left);
            if (r == 1) {
                if (profiling_enabled)
                    ++prof.msgs;
                if (sm.has_auth_response()) {
                    session_id = sm.auth_response().session_id();
                    t2d::log::info("auth_response session_id={} (len={})", session_id, session_id.size());
                } else if (sm.has_match_start()) {
                    in_match = true;
                    timing->setMatchActive(true);
                    tickRate = sm.match_start().tick_rate();
                    if (tickRate > 0) {
                        timing->setTickIntervalMs((int)(1000 / tickRate));
                        iteration_budget = std::chrono::milliseconds(1000 / tickRate);
                    } else {
                        iteration_budget = std::chrono::milliseconds(20);
                    }
                    input_interval = iteration_budget / 2; // keep inputs at 2x tick cadence
                    myEntityId = sm.match_start().my_entity_id();
                    timing->setMyEntityId(myEntityId);
                    t2d::log::info(
                        "match_start received match_id={} my_entity_id={} tick_rate={} initial_players={} "
                        "disable_bot_fire={} iteration_budget_ms={}",
                        sm.match_start().match_id(),
                        myEntityId,
                        tickRate,
                        sm.match_start().initial_player_count(),
                        sm.match_start().disable_bot_fire(),
                        iteration_budget.count());
                    uint32_t ipc = sm.match_start().initial_player_count();
                    bool dbf = sm.match_start().disable_bot_fire();
                    uint64_t secs = (ipc <= 1) ? 120ull : (dbf ? 300ull : 60ull);
                    hardCapTicks = tickRate * secs;
                    matchStartServerTick = 0;
                    timing->setHardCap(matchStartServerTick, tickRate, hardCapTicks);
                } else if (sm.has_snapshot()) {
                    tankModel->applyFull(sm.snapshot());
                    projModel->applyFull(sm.snapshot());
                    ammoModel->applyFull(sm.snapshot());
                    crateModel->applyFull(sm.snapshot());
                    timing->markServerTick();
                    timing->setServerTick(sm.snapshot().server_tick());
                } else if (sm.has_delta_snapshot()) {
                    tankModel->applyDelta(sm.delta_snapshot());
                    projModel->applyDelta(sm.delta_snapshot());
                    crateModel->applyDelta(sm.delta_snapshot());
                    timing->markServerTick();
                    timing->setServerTick(sm.delta_snapshot().server_tick());
                } else if (sm.has_match_end()) {
                    t2d::log::info(
                        "match_end received winner_entity={} my_entity={} server_tick={}",
                        sm.match_end().winner_entity_id(),
                        myEntityId,
                        sm.match_end().server_tick());
                    timing->onMatchEnd(sm.match_end().winner_entity_id(), myEntityId);
                    in_match = false;
                    timing->setMatchActive(false);
                } else if (sm.has_queue_status()) {
                    if (lobby)
                        lobby->updateFromQueue(sm.queue_status());
                }
            } else if (r == -1) {
                // Connection closed or fatal parse -> exit loop
                break;
            } else { // r == 0
                // Optionally sleep away remaining budget cooperatively
                auto after_recv = std::chrono::steady_clock::now();
                auto used = after_recv - iter_start;
                auto leftover = (used >= iteration_budget)
                    ? std::chrono::milliseconds(0)
                    : std::chrono::duration_cast<std::chrono::milliseconds>(iteration_budget - used);
                if (leftover.count() > 0)
                    co_await sched->yield_for(leftover);
            }
        }
        ++loop_iter; // diagnostic counter (no longer drives timing)
        if (profiling_enabled) {
            auto iter_end = std::chrono::steady_clock::now();
            double loop_ms = std::chrono::duration<double, std::milli>(iter_end - iter_start).count();
            prof.loop_time_acc_ms += loop_ms;
            ++prof.loops;
            // msgs counted inside drain loop when r==1
            // Emit every ~5s
            auto win_elapsed = iter_end - prof.window_start;
            if (win_elapsed >= std::chrono::seconds(5)) {
                double avg_loop = prof.loops ? (prof.loop_time_acc_ms / prof.loops) : 0.0;
                std::ostringstream _avg_loop_ss;
                _avg_loop_ss.setf(std::ios::fixed, std::ios::floatfield);
                _avg_loop_ss << std::setprecision(3) << avg_loop;
                t2d::log::info(
                    "prof window=5s loops={} avg_loop_ms={} msgs={} inputs={} heartbeats={}",
                    prof.loops,
                    _avg_loop_ss.str(),
                    prof.msgs,
                    prof.inputs,
                    prof.heartbeats);
                prof.window_start = iter_end;
                prof.loops = prof.msgs = prof.inputs = prof.heartbeats = 0;
                prof.loop_time_acc_ms = 0.0;
            }
        }
        // Timing frame progression now driven exclusively on UI thread (QML Timer) to avoid races.
        if (timing->consumeRequeueRequest()) {
            // Reset state for next match
            in_match = false;
            myEntityId = 0;
            t2d::ClientMessage qj;
            auto *qjoin = qj.mutable_queue_join();
            if (!session_id.empty())
                qjoin->set_session_id(session_id);
            co_await send_frame(cli, qj);
            t2d::log::info("requeue requested");
        }
        // End of loop: if we still drifted and performed little work, yield cooperatively
        co_await sched->yield();
    }
    t2d::log::info("qt_client network loop exit");
}
} // namespace

int main(int argc, char **argv)
{
    std::signal(SIGINT, handle_sig);
    std::signal(SIGTERM, handle_sig);
    // Allow override via --log-level=LEVEL (debug|info|warn|error) if env not already set.
    if (std::getenv("T2D_LOG_LEVEL") == nullptr) {
        for (int i = 1; i < argc; ++i) {
            const char prefix[] = "--log-level=";
            if (std::strncmp(argv[i], prefix, sizeof(prefix) - 1) == 0) {
                const char *val = argv[i] + (sizeof(prefix) - 1);
                if (*val) {
                    setenv("T2D_LOG_LEVEL", val, 1);
                }
                // Compact argv by removing this argument so Qt does not see it.
                for (int j = i; j + 1 < argc; ++j) {
                    argv[j] = argv[j + 1];
                }
                --argc;
                break;
            }
        }
        // If still unset, default to 'info'. (Previously forced to debug; now respectful.)
        if (std::getenv("T2D_LOG_LEVEL") == nullptr) {
            setenv("T2D_LOG_LEVEL", "info", 1);
        }
    }
    // Extract optional auth stub prefix (env has priority, then CLI arg --auth-stub-prefix=PREFIX)
    std::string auth_stub_prefix;
    if (const char *env_pref = std::getenv("T2D_AUTH_STUB_PREFIX"); env_pref && *env_pref) {
        auth_stub_prefix = env_pref;
    }
    // Allow overriding server host/port (defaults 127.0.0.1:40000)
    std::string server_host = "127.0.0.1";
    uint16_t server_port = 40000;
    for (int i = 1; i < argc; ++i) {
        const char aprefix[] = "--auth-stub-prefix=";
        const char hpfx[] = "--server-host=";
        const char ppfx[] = "--server-port=";
        if (std::strncmp(argv[i], aprefix, sizeof(aprefix) - 1) == 0) {
            if (auth_stub_prefix.empty()) {
                const char *val = argv[i] + (sizeof(aprefix) - 1);
                if (*val)
                    auth_stub_prefix = val;
            }
            for (int j = i; j + 1 < argc; ++j)
                argv[j] = argv[j + 1];
            --argc;
            --i;
            continue;
        } else if (std::strncmp(argv[i], hpfx, sizeof(hpfx) - 1) == 0) {
            const char *val = argv[i] + (sizeof(hpfx) - 1);
            if (*val)
                server_host = val;
            for (int j = i; j + 1 < argc; ++j)
                argv[j] = argv[j + 1];
            --argc;
            --i;
            continue;
        } else if (std::strncmp(argv[i], ppfx, sizeof(ppfx) - 1) == 0) {
            const char *val = argv[i] + (sizeof(ppfx) - 1);
            if (*val) {
                try {
                    int p = std::stoi(val);
                    if (p > 0 && p < 65536)
                        server_port = static_cast<uint16_t>(p);
                } catch (...) {
                    // ignore invalid
                }
            }
            for (int j = i; j + 1 < argc; ++j)
                argv[j] = argv[j + 1];
            --argc;
            --i;
            continue;
        }
    }
    t2d::log::init();
    QGuiApplication app(argc, argv);
    EntityModel tankModel; // tanks
    ProjectileModel projectileModel; // projectiles
    AmmoBoxModel ammoBoxModel; // ammo pickups
    CrateModel crateModel; // movable crates
    InputState input;
    TimingState timing;
    QQmlApplicationEngine engine;
    LobbyState lobby; // lobby state (must be set before engine.load so QML bindings find it)
    engine.rootContext()->setContextProperty("entityModel", &tankModel);
    engine.rootContext()->setContextProperty("projectileModel", &projectileModel);
    engine.rootContext()->setContextProperty("inputState", &input);
    engine.rootContext()->setContextProperty("ammoBoxModel", &ammoBoxModel);
    engine.rootContext()->setContextProperty("crateModel", &crateModel);
    engine.rootContext()->setContextProperty("timingState", &timing);
    engine.rootContext()->setContextProperty("lobbyState", &lobby);
    // The generated resource uses path prefix including source-relative directories; load via full embedded path.
    engine.load(QUrl("qrc:/T2DClient/src/client/qt/qml/Main.qml"));
    if (engine.rootObjects().isEmpty()) {
        t2d::log::error("Failed to load QML scene");
        return 1;
    }
    // Attempt to enable vsync pacing (replaces QTimer fractional pacing). Falls back automatically if window cast
    // fails.
    if (!engine.rootObjects().isEmpty()) {
        if (auto *w = qobject_cast<QQuickWindow *>(engine.rootObjects().first())) {
            // Enable vsync pacing first; start() will early-return due to usingVsync_ flag.
            QMetaObject::invokeMethod(&timing, [&, w]() { timing.enableVsyncPacing(w); });
        }
    }
    // Kick off internal timing driver (if vsync enabled, start() is a no-op except setting period).
    QMetaObject::invokeMethod(&timing, "start", Qt::QueuedConnection);
    // Connect map dimension changes to QML root (rootItem inside Window) so boundary draws automatically.
    QObject *rootObj = engine.rootObjects().first();
    if (rootObj) {
        // rootItem is the first child Item inside the Window (id=rootItem). Find by objectName if set or fallback to
        // findChild.
        QObject *rootItem = rootObj->findChild<QObject *>("rootItem", Qt::FindChildrenRecursively);
        if (rootItem) {
            QObject::connect(
                &tankModel,
                &EntityModel::mapDimensionsChanged,
                rootItem,
                [&tankModel, rootItem]()
                {
                    rootItem->setProperty("mapWidth", tankModel.mapWidth());
                    rootItem->setProperty("mapHeight", tankModel.mapHeight());
                    t2d::log::info("map dimensions received w={} h={}", tankModel.mapWidth(), tankModel.mapHeight());
                });
        }
    }
    // Construct oauth token from prefix (prefix + "qt" + pid) or fallback constant
    std::string oauth_token = "qt_ui_dummy";
    if (!auth_stub_prefix.empty()) {
        oauth_token = auth_stub_prefix + "qt" + std::to_string(::getpid());
    }
    auto sched = coro::default_executor::io_executor();
    sched->spawn(run_network(
        sched,
        &tankModel,
        &projectileModel,
        &ammoBoxModel,
        &crateModel,
        &input,
        &timing,
        &lobby,
        server_host,
        server_port,
        oauth_token));
    int rc = app.exec();
    g_shutdown.store(true);
    return rc;
}
