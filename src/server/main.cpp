#include "common/logger.hpp"
#include "common/metrics.hpp"
#include "server/matchmaking/matchmaker.hpp"
#include "server/matchmaking/session_manager.hpp"
#include "server/net/listener.hpp"

#include <coro/default_executor.hpp>
#include <coro/io_scheduler.hpp>
#include <yaml-cpp/yaml.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <memory>
#include <thread>

namespace t2d {
extern std::atomic_bool g_shutdown;
}

static coro::task<void> heartbeat_monitor(std::shared_ptr<coro::io_scheduler> sched, uint32_t timeout_sec)
{
    co_await sched->schedule();
    using clock = std::chrono::steady_clock;
    while (!t2d::g_shutdown.load()) {
        auto now = clock::now();
        auto sessions = t2d::mm::instance().snapshot_all_sessions();
        for (auto &s : sessions) {
            if (s->last_heartbeat.time_since_epoch().count() == 0)
                continue;
            auto diff = std::chrono::duration_cast<std::chrono::seconds>(now - s->last_heartbeat).count();
            if (diff > timeout_sec) {
                t2d::log::warn("[hb] disconnect timeout session={} diff={}s", s->session_id, diff);
                t2d::mm::instance().disconnect_session(s);
            }
        }
        co_await sched->yield_for(std::chrono::seconds(5));
    }
    co_return;
}

// Entry point for the authoritative game server.
// NOTE: Networking, matchmaking, and game loops are skeletal placeholders.

namespace t2d {
struct ServerConfig
{
    uint32_t max_players_per_match{16};
    uint32_t max_parallel_matches{4};
    uint32_t queue_soft_limit{256};
    uint32_t fill_timeout_seconds{180};
    uint32_t tick_rate{30};
    uint32_t snapshot_interval_ticks{5};
    uint32_t full_snapshot_interval_ticks{30};
    uint16_t listen_port{40000};
    uint32_t heartbeat_timeout_seconds{30};
    uint32_t matchmaker_poll_ms{200};
};

static ServerConfig load_config(const std::string &path)
{
    YAML::Node root = YAML::LoadFile(path);
    ServerConfig cfg;
    if (root["max_players_per_match"])
        cfg.max_players_per_match = root["max_players_per_match"].as<uint32_t>();
    if (root["max_parallel_matches"])
        cfg.max_parallel_matches = root["max_parallel_matches"].as<uint32_t>();
    if (root["queue_soft_limit"])
        cfg.queue_soft_limit = root["queue_soft_limit"].as<uint32_t>();
    if (root["fill_timeout_seconds"])
        cfg.fill_timeout_seconds = root["fill_timeout_seconds"].as<uint32_t>();
    if (root["tick_rate"])
        cfg.tick_rate = root["tick_rate"].as<uint32_t>();
    if (root["snapshot_interval_ticks"])
        cfg.snapshot_interval_ticks = root["snapshot_interval_ticks"].as<uint32_t>();
    if (root["full_snapshot_interval_ticks"])
        cfg.full_snapshot_interval_ticks = root["full_snapshot_interval_ticks"].as<uint32_t>();
    if (root["listen_port"])
        cfg.listen_port = root["listen_port"].as<uint16_t>();
    if (root["heartbeat_timeout_seconds"])
        cfg.heartbeat_timeout_seconds = root["heartbeat_timeout_seconds"].as<uint32_t>();
    if (root["matchmaker_poll_ms"])
        cfg.matchmaker_poll_ms = root["matchmaker_poll_ms"].as<uint32_t>();
    return cfg;
}

std::atomic_bool g_shutdown{false};
} // namespace t2d

static void handle_signal(int)
{
    t2d::g_shutdown.store(true);
    t2d::log::info("Signal received, shutting down...");
}

int main(int argc, char **argv)
{
    std::string config_path = "config/server.yaml";
    if (argc > 1)
        config_path = argv[1];

    t2d::ServerConfig cfg;
    try {
        cfg = t2d::load_config(config_path);
    } catch (const std::exception &ex) {
        t2d::log::error("Failed to load config: {}", ex.what());
        return 1;
    }

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    t2d::log::info("t2d server starting (version: {})", T2D_VERSION);
    t2d::log::info("Tick rate: {} Hz", cfg.tick_rate);
    t2d::log::info("Listening on port: {}", cfg.listen_port);

    // io_scheduler requires options; construct explicitly
    auto scheduler = coro::default_executor::io_executor();
    // Spawn TCP listener coroutine
    scheduler->spawn(t2d::net::run_listener(scheduler, cfg.listen_port));
    // Launch matchmaker coroutine
    scheduler->spawn(t2d::mm::run_matchmaker(
        scheduler,
        t2d::mm::MatchConfig{
            cfg.max_players_per_match,
            cfg.fill_timeout_seconds,
            cfg.tick_rate,
            cfg.matchmaker_poll_ms,
            cfg.snapshot_interval_ticks,
            cfg.full_snapshot_interval_ticks}));
    // Launch heartbeat monitor
    scheduler->spawn(heartbeat_monitor(scheduler, cfg.heartbeat_timeout_seconds));

    // Main thread just sleeps; real implementation will add signal handling & graceful shutdown.
    while (!t2d::g_shutdown.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    t2d::log::info("Shutdown complete.");
    // Dump snapshot metrics (stdout JSON lines if JSON mode enabled externally in logger)
    auto fullB = t2d::metrics::snapshot().full_bytes.load();
    auto deltaB = t2d::metrics::snapshot().delta_bytes.load();
    auto fullC = t2d::metrics::snapshot().full_count.load();
    auto deltaC = t2d::metrics::snapshot().delta_count.load();
    t2d::log::info(
        "{\"metric\":\"snapshot_totals\",\"full_bytes\":{} ,\"delta_bytes\":{} ,\"full_count\":{} ,\"delta_count\":{}}",
        fullB,
        deltaB,
        fullC,
        deltaC);
    return 0;
}
