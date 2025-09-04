// SPDX-License-Identifier: Apache-2.0
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
            // Ignore bots for heartbeat timeouts to allow persistent automated matches.
            if (s->is_bot)
                continue;
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

// Periodic resource sampler: captures user CPU time and RSS peak approximation.
static coro::task<void> resource_sampler(std::shared_ptr<coro::io_scheduler> sched)
{
    co_await sched->schedule();
    using clock = std::chrono::steady_clock;
    // Track previous process times to compute deltas.
    long clk_tck = sysconf(_SC_CLK_TCK);
    if (clk_tck <= 0)
        clk_tck = 100; // fallback
    auto last_wall = clock::now();
    uint64_t last_utime_ticks = 0, last_stime_ticks = 0;
    bool first = true;
    while (!t2d::g_shutdown.load()) {
        // Read /proc/self/stat (utime=14, stime=15 fields)
        FILE *f = std::fopen("/proc/self/stat", "r");
        if (f) {
            char buf[4096];
            if (std::fgets(buf, sizeof(buf), f)) {
                // Tokenize carefully (second field can contain spaces inside parentheses). Simplistic approach: find
                // last ')' then split.
                char *rp = strrchr(buf, ')');
                if (rp) {
                    // count fields after rp+2
                    int field = 3; // we start counting after pid and comm
                    char *p = rp + 2;
                    char *save = nullptr;
                    uint64_t ut = 0, st = 0;
                    for (char *tok = strtok_r(p, " ", &save); tok; tok = strtok_r(nullptr, " ", &save)) {
                        if (field == 14)
                            ut = strtoull(tok, nullptr, 10);
                        if (field == 15)
                            st = strtoull(tok, nullptr, 10);
                        if (field > 15)
                            break;
                        ++field;
                    }
                    if (ut || st) {
                        if (first) {
                            last_utime_ticks = ut;
                            last_stime_ticks = st;
                            first = false;
                        } else {
                            auto now = clock::now();
                            auto wall_ns =
                                std::chrono::duration_cast<std::chrono::nanoseconds>(now - last_wall).count();
                            last_wall = now;
                            uint64_t dut = (ut - last_utime_ticks);
                            uint64_t dst = (st - last_stime_ticks);
                            last_utime_ticks = ut;
                            last_stime_ticks = st;
                            uint64_t cpu_ns = (dut + dst) * (1000000000ull / (uint64_t)clk_tck);
                            auto &rt = t2d::metrics::runtime();
                            rt.user_cpu_ns_accum.fetch_add(cpu_ns, std::memory_order_relaxed);
                            rt.wall_clock_ns_accum.fetch_add(wall_ns, std::memory_order_relaxed);
                        }
                    }
                }
            }
            std::fclose(f);
        }
        // RSS current from /proc/self/statm (2nd field resident pages)
        FILE *fm = std::fopen("/proc/self/statm", "r");
        if (fm) {
            unsigned long pages_total = 0, pages_res = 0;
            if (fscanf(fm, "%lu %lu", &pages_total, &pages_res) == 2) {
                long page_sz = sysconf(_SC_PAGESIZE);
                if (page_sz <= 0)
                    page_sz = 4096;
                uint64_t rss_bytes = (uint64_t)pages_res * (uint64_t)page_sz;
                auto &rt = t2d::metrics::runtime();
                uint64_t prev = rt.rss_peak_bytes.load(std::memory_order_relaxed);
                while (rss_bytes > prev
                       && !rt.rss_peak_bytes.compare_exchange_weak(prev, rss_bytes, std::memory_order_relaxed)) {
                    // loop
                }
            }
            std::fclose(fm);
        }
        co_await sched->yield_for(std::chrono::seconds(1));
    }
    co_return;
}

// Entry point for the authoritative game server.
// NOTE: Networking, matchmaking, and game loops are skeletal placeholders.

namespace t2d {
struct ServerConfig
{
    // Defaults aligned with server_test.yaml (test-optimized profile)
    uint32_t max_players_per_match{4};
    uint32_t max_parallel_matches{4};
    uint32_t queue_soft_limit{64};
    uint32_t fill_timeout_seconds{2};
    uint32_t tick_rate{30};
    uint32_t snapshot_interval_ticks{5};
    uint32_t full_snapshot_interval_ticks{30};
    uint16_t listen_port{40001};
    uint32_t heartbeat_timeout_seconds{15};
    uint32_t matchmaker_poll_ms{100};
    std::string log_level{"debug"};
    bool log_json{false};
    uint16_t metrics_port{0}; // 0 disables
    std::string auth_mode{"stub"};
    std::string auth_stub_prefix{"test_user_"};
    uint32_t bot_fire_interval_ticks{5};
    float movement_speed{2.5f};
    uint32_t projectile_damage{50};
    float reload_interval_sec{1.5f};
    float projectile_speed{10.0f};
    float projectile_density{20.0f};
    float projectile_max_lifetime_sec{5.0f};
    float fire_cooldown_sec{0.25f};
    float hull_density{5.0f};
    float turret_density{2.5f};
    bool disable_bot_fire{false};
    bool disable_bot_ai{false};
    bool test_mode{true};
    // Map dimensions (world playable area). Walls will be created at perimeter.
    float map_width{80.f};
    float map_height{80.f};
    // Test/diagnostic hook: when true, spawn players in a horizontal line (even spacing) instead of random.
    bool force_line_spawn{false};
    // When true, destroyed tanks remain in the world (corpses) until match end (do not remove from snapshots).
    bool persist_destroyed_tanks{false};
    uint32_t track_break_hits{1};
    uint32_t turret_disable_front_hits{2};
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
    if (root["projectile_density"]) {
        cfg.projectile_density = root["projectile_density"].as<float>();
    }
    if (root["projectile_max_lifetime_sec"]) {
        cfg.projectile_max_lifetime_sec = root["projectile_max_lifetime_sec"].as<float>();
    }
    if (root["fire_cooldown_sec"]) {
        cfg.fire_cooldown_sec = root["fire_cooldown_sec"].as<float>();
    }
    if (root["hull_density"]) {
        cfg.hull_density = root["hull_density"].as<float>();
    }
    if (root["turret_density"]) {
        cfg.turret_density = root["turret_density"].as<float>();
    }
    if (root["disable_bot_fire"]) {
        cfg.disable_bot_fire = root["disable_bot_fire"].as<bool>();
    }
    if (root["disable_bot_ai"]) {
        cfg.disable_bot_ai = root["disable_bot_ai"].as<bool>();
    }
    if (root["test_mode"]) {
        cfg.test_mode = root["test_mode"].as<bool>();
    }
    if (root["map_width"]) {
        cfg.map_width = root["map_width"].as<float>();
    }
    if (root["map_height"]) {
        cfg.map_height = root["map_height"].as<float>();
    }
    if (root["force_line_spawn"]) {
        cfg.force_line_spawn = root["force_line_spawn"].as<bool>();
    }
    if (root["persist_destroyed_tanks"]) {
        cfg.persist_destroyed_tanks = root["persist_destroyed_tanks"].as<bool>();
    }
    if (root["track_break_hits"]) {
        cfg.track_break_hits = root["track_break_hits"].as<uint32_t>();
    }
    if (root["turret_disable_front_hits"]) {
        cfg.turret_disable_front_hits = root["turret_disable_front_hits"].as<uint32_t>();
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
    t2d::ServerConfig cfg; // allocate early so CLI flags can set fields before/after file load
    std::string config_path = "config/server.yaml";
    bool cli_disable_bot_fire = false;
    bool cli_disable_bot_ai = false;
    bool cli_port_override = false;
    uint16_t port_override = 0;
    int duration_override_sec = 0; // 0 means run until signal
    bool auto_test_match = false; // enqueue bots immediately to form a match
    // Simple arg parsing: first non-flag = config path; recognize --no-bot-fire / --no-bot-ai
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--no-bot-fire") {
            cli_disable_bot_fire = true;
        } else if (a == "--no-bot-ai") {
            cli_disable_bot_ai = true;
        } else if (a == "--port" && i + 1 < argc) {
            try {
                port_override = static_cast<uint16_t>(std::stoi(argv[++i]));
                cli_port_override = true;
            } catch (...) {
                t2d::log::warn("Invalid --port value '{}', ignoring", argv[i]);
            }
        } else if (a == "--duration" && i + 1 < argc) {
            try {
                duration_override_sec = std::stoi(argv[++i]);
            } catch (...) {
                t2d::log::warn("Invalid --duration value '{}', ignoring", argv[i]);
            }
        } else if (a == "--auto-test-match") {
            auto_test_match = true;
        } else if (!a.empty() && a[0] != '-') {
            config_path = a;
        }
    }
    try {
        cfg = t2d::load_config(config_path);
        if (std::getenv("T2D_NO_BOT_FIRE")) {
            cfg.disable_bot_fire = true;
        }
        if (cli_disable_bot_fire) {
            cfg.disable_bot_fire = true;
        }
        if (std::getenv("T2D_NO_BOT_AI")) {
            cfg.disable_bot_ai = true;
        }
        if (cli_disable_bot_ai) {
            cfg.disable_bot_ai = true;
        }
    } catch (const std::exception &ex) {
        t2d::log::error("Failed to load config: {}", ex.what());
        return 1;
    }

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    // Apply logging config via environment emulation before first log init.
    // Do NOT override an explicit external setting (e.g. run_dev_loop --log-level).
    if (!cfg.log_level.empty() && std::getenv("T2D_LOG_LEVEL") == nullptr) {
        setenv("T2D_LOG_LEVEL", cfg.log_level.c_str(), 1);
    }
    if (cfg.log_json) {
        setenv("T2D_LOG_JSON", "1", 1);
    }
    t2d::log::init();
    t2d::log::info(
        "t2d server starting (version: {} sha:{} dirty:{} build:{})",
        T2D_VERSION,
        T2D_GIT_SHA,
        T2D_BUILD_DIRTY,
        T2D_BUILD_DATE);
    t2d::log::info("Profiling macro T2D_PROFILING_ENABLED={}", T2D_PROFILING_ENABLED);
    if (cli_port_override) {
        cfg.listen_port = port_override;
        t2d::log::info("CLI override: listen_port set to {}", cfg.listen_port);
    }
    if (duration_override_sec > 0) {
        t2d::log::info("CLI override: auto-shutdown after {} seconds", duration_override_sec);
    }
    t2d::log::info("Tick rate: {} Hz", cfg.tick_rate);
    t2d::log::info("Listening on port: {}", cfg.listen_port);
    t2d::log::info("Auth mode: {}", cfg.auth_mode);
    if (cfg.disable_bot_fire) {
        t2d::log::info("Bot firing disabled (--no-bot-fire)");
    }
    if (cfg.disable_bot_ai) {
        t2d::log::info("Bot AI disabled (--no-bot-ai)");
    }

    // io_scheduler requires options; construct explicitly
    auto scheduler = coro::default_executor::io_executor();
    // Spawn TCP listener coroutine (pass tick_rate for adaptive connection poll timeouts)
    scheduler->spawn(t2d::net::run_listener(scheduler, cfg.listen_port, cfg.tick_rate));
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
            cfg.projectile_speed,
            cfg.projectile_density,
            cfg.projectile_max_lifetime_sec,
            cfg.fire_cooldown_sec,
            cfg.hull_density,
            cfg.turret_density,
            cfg.disable_bot_fire,
            cfg.disable_bot_ai,
            cfg.test_mode,
            cfg.map_width,
            cfg.map_height,
            cfg.force_line_spawn,
            cfg.persist_destroyed_tanks}));
    // Launch heartbeat monitor
    scheduler->spawn(heartbeat_monitor(scheduler, cfg.heartbeat_timeout_seconds));
    // Launch resource sampler (profiling / production lightweight)
    scheduler->spawn(resource_sampler(scheduler));
    if (cfg.metrics_port != 0) {
        scheduler->spawn(t2d::net::run_metrics_endpoint(scheduler, cfg.metrics_port));
    }
    // Initialize auth provider (lifetime static); store pointer for listener usage
    static auto auth_provider_storage = t2d::auth::make_provider(cfg.auth_mode, cfg.auth_stub_prefix);
    t2d::auth::set_provider(auth_provider_storage.get());

    if (auto_test_match) {
        // Pre-fill matchmaking queue with bots so first poll creates a match quickly.
        auto &mgr = t2d::mm::instance();
        auto created = mgr.create_bots(cfg.max_players_per_match);
        t2d::log::info("Auto test match enabled: queued {} bots to trigger immediate match", created.size());
    }

    // Main thread just sleeps; real implementation will add signal handling & graceful shutdown.
    auto run_start = std::chrono::steady_clock::now();
    while (!t2d::g_shutdown.load()) {
        static auto last_metrics = std::chrono::steady_clock::now();
        std::this_thread::sleep_for(std::chrono::seconds(1));
        auto now = std::chrono::steady_clock::now();
        if (duration_override_sec > 0) {
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - run_start).count();
            if (elapsed >= duration_override_sec) {
                t2d::log::info("Duration reached ({}s >= {}s); initiating shutdown", elapsed, duration_override_sec);
                t2d::g_shutdown.store(true);
            }
        }
        if (now - last_metrics >= std::chrono::seconds(60)) {
            last_metrics = now;
            auto &rt = t2d::metrics::runtime();
            uint64_t samples = rt.tick_samples.load();
            uint64_t avg_ns = samples ? rt.tick_duration_ns_accum.load() / samples : 0;
            uint64_t p99_ns = t2d::metrics::approx_tick_p99();
            // Compute user CPU % over accumulated window if wall_clock_ns_accum > 0
            uint64_t user_cpu_ns = rt.user_cpu_ns_accum.load(std::memory_order_relaxed);
            uint64_t wall_ns = rt.wall_clock_ns_accum.load(std::memory_order_relaxed);
            double cpu_pct = (wall_ns > 0) ? (100.0 * (double)user_cpu_ns / (double)wall_ns) : 0.0;
            uint64_t wait_p99_ns = t2d::metrics::approx_wait_p99();
            double allocs_per_tick_mean = 0.0;
            auto alloc_samples = rt.allocations_per_tick_samples.load(std::memory_order_relaxed);
            if (alloc_samples > 0) {
                allocs_per_tick_mean =
                    (double)rt.allocations_per_tick_accum.load(std::memory_order_relaxed) / (double)alloc_samples;
            }
            {
                std::ostringstream j;
                j.setf(std::ios::fixed);
                j.precision(2);
                j << "{\"metric\":\"runtime\"";
                j << ",\"avg_tick_ns\":" << avg_ns;
                j << ",\"p99_tick_ns\":" << p99_ns;
                j << ",\"wait_p99_ns\":" << wait_p99_ns;
                // Mean wait between ticks (scheduler idle) derived from accumulated sum
                uint64_t wait_samples = rt.wait_samples.load(std::memory_order_relaxed);
                uint64_t wait_mean_ns =
                    wait_samples ? (rt.wait_duration_ns_accum.load(std::memory_order_relaxed) / wait_samples) : 0;
                j << ",\"wait_mean_ns\":" << wait_mean_ns;
                j << ",\"cpu_user_pct\":" << cpu_pct;
                j << ",\"rss_peak_bytes\":" << rt.rss_peak_bytes.load(std::memory_order_relaxed);
                j << ",\"allocs_per_tick_mean\":" << allocs_per_tick_mean;
                // Derived metrics: bytes per tick mean & alloc frequency
                double alloc_bytes_mean = 0.0;
                auto ab_samples = rt.allocations_bytes_per_tick_samples.load(std::memory_order_relaxed);
                if (ab_samples > 0) {
                    alloc_bytes_mean = (double)rt.allocations_bytes_per_tick_accum.load(std::memory_order_relaxed)
                        / (double)ab_samples;
                }
                double alloc_tick_pct = 0.0;
                auto ticks_with_alloc = rt.allocations_ticks_with_alloc.load(std::memory_order_relaxed);
                if (rt.allocations_per_tick_samples.load(std::memory_order_relaxed) > 0) {
                    alloc_tick_pct = 100.0 * (double)ticks_with_alloc
                        / (double)rt.allocations_per_tick_samples.load(std::memory_order_relaxed);
                }
                j << ",\"alloc_bytes_per_tick_mean\":" << alloc_bytes_mean;
                j << ",\"alloc_tick_with_alloc_pct\":" << alloc_tick_pct;
                // Deallocation stats
                double frees_per_tick_mean = 0.0;
                auto free_samples = rt.deallocations_per_tick_samples.load(std::memory_order_relaxed);
                if (free_samples > 0) {
                    frees_per_tick_mean =
                        (double)rt.deallocations_per_tick_accum.load(std::memory_order_relaxed) / (double)free_samples;
                }
                double free_tick_pct = 0.0;
                auto ticks_with_free = rt.deallocations_ticks_with_free.load(std::memory_order_relaxed);
                if (free_samples > 0) {
                    free_tick_pct = 100.0 * (double)ticks_with_free / (double)free_samples;
                }
                j << ",\"frees_per_tick_mean\":" << frees_per_tick_mean;
                j << ",\"free_tick_with_free_pct\":" << free_tick_pct;
                j << ",\"queue_depth\":" << rt.queue_depth.load();
                j << ",\"active_matches\":" << rt.active_matches.load();
                j << ",\"bots_in_match\":" << rt.bots_in_match.load();
                j << ",\"projectiles_active\":" << rt.projectiles_active.load();
                j << ",\"connected_players\":" << rt.connected_players.load();
                j << "}";
                t2d::log::info("{}", j.str());
            }
        }
    }
    t2d::log::info("Shutdown complete.");
    // Final runtime metrics flush (ensures short profiling runs capture avg tick stats)
    {
        auto &rt = t2d::metrics::runtime();
        uint64_t samples = rt.tick_samples.load();
        uint64_t avg_ns = samples ? rt.tick_duration_ns_accum.load() / samples : 0;
        uint64_t p99_ns = t2d::metrics::approx_tick_p99();
        uint64_t wait_p99_ns = t2d::metrics::approx_wait_p99();
        uint64_t user_cpu_ns = rt.user_cpu_ns_accum.load(std::memory_order_relaxed);
        uint64_t wall_ns = rt.wall_clock_ns_accum.load(std::memory_order_relaxed);
        double cpu_pct = (wall_ns > 0) ? (100.0 * (double)user_cpu_ns / (double)wall_ns) : 0.0;
        double allocs_per_tick_mean = 0.0;
        auto alloc_samples = rt.allocations_per_tick_samples.load(std::memory_order_relaxed);
        if (alloc_samples > 0) {
            allocs_per_tick_mean =
                (double)rt.allocations_per_tick_accum.load(std::memory_order_relaxed) / (double)alloc_samples;
        }
        {
            std::ostringstream j;
            j.setf(std::ios::fixed);
            j.precision(2);
            j << "{\"metric\":\"runtime_final\"";
            j << ",\"avg_tick_ns\":" << avg_ns;
            j << ",\"p99_tick_ns\":" << p99_ns;
            j << ",\"wait_p99_ns\":" << wait_p99_ns;
            uint64_t wait_samples_final = rt.wait_samples.load(std::memory_order_relaxed);
            uint64_t wait_mean_ns_final = wait_samples_final
                ? (rt.wait_duration_ns_accum.load(std::memory_order_relaxed) / wait_samples_final)
                : 0;
            j << ",\"wait_mean_ns\":" << wait_mean_ns_final;
            j << ",\"cpu_user_pct\":" << cpu_pct;
            j << ",\"rss_peak_bytes\":" << rt.rss_peak_bytes.load(std::memory_order_relaxed);
            j << ",\"allocs_per_tick_mean\":" << allocs_per_tick_mean;
            uint64_t allocs_p95 = t2d::metrics::approx_allocations_per_tick_p95();
            j << ",\"allocs_per_tick_p95\":" << allocs_p95;
            double alloc_bytes_mean = 0.0;
            auto ab_samples = rt.allocations_bytes_per_tick_samples.load(std::memory_order_relaxed);
            if (ab_samples > 0) {
                alloc_bytes_mean =
                    (double)rt.allocations_bytes_per_tick_accum.load(std::memory_order_relaxed) / (double)ab_samples;
            }
            double alloc_tick_pct = 0.0;
            auto ticks_with_alloc = rt.allocations_ticks_with_alloc.load(std::memory_order_relaxed);
            if (rt.allocations_per_tick_samples.load(std::memory_order_relaxed) > 0) {
                alloc_tick_pct = 100.0 * (double)ticks_with_alloc
                    / (double)rt.allocations_per_tick_samples.load(std::memory_order_relaxed);
            }
            j << ",\"alloc_bytes_per_tick_mean\":" << alloc_bytes_mean;
            j << ",\"alloc_tick_with_alloc_pct\":" << alloc_tick_pct;
#if T2D_PROFILING_ENABLED
            double scratch_reuse = t2d::metrics::snapshot_scratch_reuse_pct();
            j << ",\"snapshot_scratch_reuse_pct\":" << scratch_reuse;
            j << ",\"projectile_pool_hit_pct\":" << t2d::metrics::projectile_pool_hit_pct();
            j << ",\"projectile_pool_misses\":" << t2d::metrics::projectile_pool_misses();
            {
                auto &rtm = t2d::metrics::runtime();
                uint64_t fcnt = rtm.snapshot_full_build_count.load(std::memory_order_relaxed);
                uint64_t dcnt = rtm.snapshot_delta_build_count.load(std::memory_order_relaxed);
                double fmean = fcnt
                    ? (double)rtm.snapshot_full_build_ns_accum.load(std::memory_order_relaxed) / (double)fcnt
                    : 0.0;
                double dmean = dcnt
                    ? (double)rtm.snapshot_delta_build_ns_accum.load(std::memory_order_relaxed) / (double)dcnt
                    : 0.0;
                j << ",\"snapshot_full_build_ns_mean\":" << fmean;
                j << ",\"snapshot_delta_build_ns_mean\":" << dmean;
            }
            {
                double log_lines_mean = 0.0;
                auto log_samples = rt.log_lines_per_tick_samples.load(std::memory_order_relaxed);
                if (log_samples > 0) {
                    log_lines_mean =
                        (double)rt.log_lines_per_tick_accum.load(std::memory_order_relaxed) / (double)log_samples;
                }
                j << ",\"log_lines_per_tick_mean\":" << log_lines_mean;
            }
#endif
            {
                double frees_per_tick_mean = 0.0;
                auto free_samples = rt.deallocations_per_tick_samples.load(std::memory_order_relaxed);
                if (free_samples > 0) {
                    frees_per_tick_mean =
                        (double)rt.deallocations_per_tick_accum.load(std::memory_order_relaxed) / (double)free_samples;
                }
                double free_tick_pct = 0.0;
                auto ticks_with_free = rt.deallocations_ticks_with_free.load(std::memory_order_relaxed);
                if (free_samples > 0) {
                    free_tick_pct = 100.0 * (double)ticks_with_free / (double)free_samples;
                }
                j << ",\"frees_per_tick_mean\":" << frees_per_tick_mean;
                j << ",\"free_tick_with_free_pct\":" << free_tick_pct;
            }
            j << ",\"samples\":" << samples;
            j << ",\"queue_depth\":" << rt.queue_depth.load();
            j << ",\"active_matches\":" << rt.active_matches.load();
            j << ",\"bots_in_match\":" << rt.bots_in_match.load();
            j << ",\"projectiles_active\":" << rt.projectiles_active.load();
            j << ",\"connected_players\":" << rt.connected_players.load();
            j << "}";
            t2d::log::info("{}", j.str());
        }
    }
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
