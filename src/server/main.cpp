#include "common/logger.hpp"
#include "common/metrics.hpp"
#include "server/auth/auth_provider.hpp"
#include "server/matchmaking/matchmaker.hpp"
#include "server/matchmaking/session_manager.hpp"
#include "server/net/listener.hpp"
#include "server/net/metrics_http.hpp"

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
    std::string log_level{"info"};
    bool log_json{false};
    uint16_t metrics_port{0}; // 0 disables
    std::string auth_mode{"disabled"};
    std::string auth_stub_prefix{"user_"};
    uint32_t bot_fire_interval_ticks{60}; // bot AI fires every N ticks (default 2s at 30Hz)
    float movement_speed{2.0f}; // units per second
    uint32_t projectile_damage{25};
    float reload_interval_sec{3.0f};
    float projectile_speed{5.0f};
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
    if (root["log_level"]) {
        cfg.log_level = root["log_level"].as<std::string>();
    }
    if (root["log_json"]) {
        cfg.log_json = root["log_json"].as<bool>();
    }
    if (root["metrics_port"]) {
        cfg.metrics_port = root["metrics_port"].as<uint16_t>();
    }
    if (root["auth_mode"]) {
        cfg.auth_mode = root["auth_mode"].as<std::string>();
    }
    if (root["auth_stub_prefix"]) {
        cfg.auth_stub_prefix = root["auth_stub_prefix"].as<std::string>();
    }
    if (root["bot_fire_interval_ticks"]) {
        cfg.bot_fire_interval_ticks = root["bot_fire_interval_ticks"].as<uint32_t>();
    }
    if (root["movement_speed"]) {
        cfg.movement_speed = root["movement_speed"].as<float>();
    }
    if (root["projectile_damage"]) {
        cfg.projectile_damage = root["projectile_damage"].as<uint32_t>();
    }
    if (root["reload_interval_sec"]) {
        cfg.reload_interval_sec = root["reload_interval_sec"].as<float>();
    }
    if (root["projectile_speed"]) {
        cfg.projectile_speed = root["projectile_speed"].as<float>();
    }
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

    // Apply logging config via environment emulation before first log init
    if (!cfg.log_level.empty()) {
        setenv("T2D_LOG_LEVEL", cfg.log_level.c_str(), 1);
    }
    if (cfg.log_json) {
        setenv("T2D_LOG_JSON", "1", 1);
    }
    t2d::log::init();
    t2d::log::info("t2d server starting (version: {})", T2D_VERSION);
    t2d::log::info("Tick rate: {} Hz", cfg.tick_rate);
    t2d::log::info("Listening on port: {}", cfg.listen_port);
    t2d::log::info("Auth mode: {}", cfg.auth_mode);

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
            cfg.full_snapshot_interval_ticks,
            cfg.bot_fire_interval_ticks,
            cfg.movement_speed,
            cfg.projectile_damage,
            cfg.reload_interval_sec,
            cfg.projectile_speed}));
    // Launch heartbeat monitor
    scheduler->spawn(heartbeat_monitor(scheduler, cfg.heartbeat_timeout_seconds));
    if (cfg.metrics_port != 0) {
        scheduler->spawn(t2d::net::run_metrics_endpoint(scheduler, cfg.metrics_port));
    }
    // Initialize auth provider (lifetime static); store pointer for listener usage
    static auto auth_provider_storage = t2d::auth::make_provider(cfg.auth_mode, cfg.auth_stub_prefix);
    t2d::auth::set_provider(auth_provider_storage.get());

    // Main thread just sleeps; real implementation will add signal handling & graceful shutdown.
    while (!t2d::g_shutdown.load()) {
        static auto last_metrics = std::chrono::steady_clock::now();
        std::this_thread::sleep_for(std::chrono::seconds(1));
        auto now = std::chrono::steady_clock::now();
        if (now - last_metrics >= std::chrono::seconds(60)) {
            last_metrics = now;
            auto &rt = t2d::metrics::runtime();
            uint64_t samples = rt.tick_samples.load();
            uint64_t avg_ns = samples ? rt.tick_duration_ns_accum.load() / samples : 0;
            t2d::log::info(
                "{\"metric\":\"runtime\",\"avg_tick_ns\":{},\"queue_depth\":{},\"active_matches\":{},\"bots_in_match\":"
                "{},\"projectiles_active\":{}}",
                avg_ns,
                rt.queue_depth.load(),
                rt.active_matches.load(),
                rt.bots_in_match.load(),
                rt.projectiles_active.load());
        }
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
