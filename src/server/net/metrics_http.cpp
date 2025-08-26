// SPDX-License-Identifier: Apache-2.0
#include "server/net/metrics_http.hpp"

#include "common/logger.hpp"
#include "common/metrics.hpp"

#include <coro/net/tcp/client.hpp>
#include <coro/net/tcp/server.hpp>
#include <coro/poll.hpp>

#include <cstring>
#include <span>
#include <sstream>
#include <string>

namespace t2d::net {

static std::string build_metrics_body()
{
    std::ostringstream oss;
    auto &snap = t2d::metrics::snapshot();
    auto &rt = t2d::metrics::runtime();
    uint64_t samples = rt.tick_samples.load();
    uint64_t avg_ns = samples ? rt.tick_duration_ns_accum.load() / samples : 0;
    uint64_t p99_ns = t2d::metrics::approx_tick_p99();
    uint64_t wait_p99_ns = t2d::metrics::approx_wait_p99();
    auto &rt_ref = rt; // alias for clarity
    uint64_t user_cpu_ns = rt_ref.user_cpu_ns_accum.load(std::memory_order_relaxed);
    uint64_t wall_ns = rt_ref.wall_clock_ns_accum.load(std::memory_order_relaxed);
    double cpu_pct = (wall_ns > 0) ? (100.0 * (double)user_cpu_ns / (double)wall_ns) : 0.0;
    double allocs_per_tick_mean = 0.0;
    auto alloc_samples = rt_ref.allocations_per_tick_samples.load(std::memory_order_relaxed);
    if (alloc_samples > 0) {
        allocs_per_tick_mean =
            (double)rt_ref.allocations_per_tick_accum.load(std::memory_order_relaxed) / (double)alloc_samples;
    }
    // Snapshot metrics
    oss << "# TYPE t2d_snapshot_full_bytes counter\n";
    oss << "t2d_snapshot_full_bytes " << snap.full_bytes.load() << "\n";
    oss << "# TYPE t2d_snapshot_delta_bytes counter\n";
    oss << "t2d_snapshot_delta_bytes " << snap.delta_bytes.load() << "\n";
    oss << "# TYPE t2d_snapshot_full_count counter\n";
    oss << "t2d_snapshot_full_count " << snap.full_count.load() << "\n";
    oss << "# TYPE t2d_snapshot_delta_count counter\n";
    oss << "t2d_snapshot_delta_count " << snap.delta_count.load() << "\n";
    // Runtime metrics (gauges)
    oss << "# TYPE t2d_queue_depth gauge\n";
    oss << "t2d_queue_depth " << rt.queue_depth.load() << "\n";
    oss << "# TYPE t2d_active_matches gauge\n";
    oss << "t2d_active_matches " << rt.active_matches.load() << "\n";
    oss << "# TYPE t2d_bots_in_match gauge\n";
    oss << "t2d_bots_in_match " << rt.bots_in_match.load() << "\n";
    oss << "# TYPE t2d_connected_players gauge\n";
    oss << "t2d_connected_players " << rt.connected_players.load() << "\n";
    oss << "# TYPE t2d_projectiles_active gauge\n";
    oss << "t2d_projectiles_active " << rt.projectiles_active.load() << "\n";
    oss << "# TYPE t2d_avg_tick_ns gauge\n";
    oss << "t2d_avg_tick_ns " << avg_ns << "\n";
    oss << "# TYPE t2d_p99_tick_ns gauge\n";
    oss << "t2d_p99_tick_ns " << p99_ns << "\n";
    oss << "# TYPE t2d_wait_p99_ns gauge\n";
    oss << "t2d_wait_p99_ns " << wait_p99_ns << "\n";
    oss << "# TYPE t2d_cpu_user_pct gauge\n";
    oss << "t2d_cpu_user_pct " << cpu_pct << "\n";
    oss << "# TYPE t2d_rss_peak_bytes gauge\n";
    oss << "t2d_rss_peak_bytes " << rt.rss_peak_bytes.load() << "\n";
    oss << "# TYPE t2d_allocs_per_tick_mean gauge\n";
    oss << "t2d_allocs_per_tick_mean " << allocs_per_tick_mean << "\n";
    // Additional allocation detail
    double alloc_bytes_mean = 0.0;
    auto ab_samples = rt.allocations_bytes_per_tick_samples.load(std::memory_order_relaxed);
    if (ab_samples > 0) {
        alloc_bytes_mean =
            (double)rt.allocations_bytes_per_tick_accum.load(std::memory_order_relaxed) / (double)ab_samples;
    }
    double alloc_tick_pct = 0.0;
    auto ticks_with_alloc = rt.allocations_ticks_with_alloc.load(std::memory_order_relaxed);
    if (rt.allocations_per_tick_samples.load(std::memory_order_relaxed) > 0) {
        alloc_tick_pct =
            100.0 * (double)ticks_with_alloc / (double)rt.allocations_per_tick_samples.load(std::memory_order_relaxed);
    }
    oss << "# TYPE t2d_alloc_bytes_per_tick_mean gauge\n";
    oss << "t2d_alloc_bytes_per_tick_mean " << alloc_bytes_mean << "\n";
    oss << "# TYPE t2d_alloc_tick_with_alloc_pct gauge\n";
    oss << "t2d_alloc_tick_with_alloc_pct " << alloc_tick_pct << "\n";
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
    oss << "# TYPE t2d_frees_per_tick_mean gauge\n";
    oss << "t2d_frees_per_tick_mean " << frees_per_tick_mean << "\n";
    oss << "# TYPE t2d_free_tick_with_free_pct gauge\n";
    oss << "t2d_free_tick_with_free_pct " << free_tick_pct << "\n";
    // Tick duration histogram (nanoseconds). Buckets are geometric (x2) starting at 250k ns (0.25ms).
    // Expose in Prometheus histogram format: *_bucket, *_sum, *_count.
    oss << "# TYPE t2d_tick_duration_ns histogram\n";
    uint64_t cumulative = 0;
    constexpr uint64_t base = 250000; // 0.25ms
    for (int i = 0; i < t2d::metrics::RuntimeCounters::TICK_BUCKETS; ++i) {
        uint64_t bcount = rt.tick_hist[i].load();
        cumulative += bcount;
        uint64_t le = (base << i);
        oss << "t2d_tick_duration_ns_bucket{le=\"" << le << "\"} " << cumulative << "\n";
    }
    // +Inf bucket
    oss << "t2d_tick_duration_ns_bucket{le=\"+Inf\"} " << cumulative << "\n";
    // Sum & count (using existing accumulators)
    oss << "t2d_tick_duration_ns_sum " << rt.tick_duration_ns_accum.load() << "\n";
    oss << "t2d_tick_duration_ns_count " << rt.tick_samples.load() << "\n";
    oss << "# TYPE t2d_auth_failures counter\n";
    oss << "t2d_auth_failures " << rt.auth_failures.load() << "\n";
    return oss.str();
}

static coro::task<void> handle_client(std::shared_ptr<coro::io_scheduler> scheduler, coro::net::tcp::client client)
{
    co_await scheduler->schedule();
    // Very small timeout; one-shot request
    auto pol = co_await client.poll(coro::poll_op::read, std::chrono::milliseconds(200));
    if (pol != coro::poll_status::event) {
        co_return;
    }
    std::string buf(1024, '\0');
    auto [rs, span] = client.recv(buf);
    if (rs != coro::net::recv_status::ok && rs != coro::net::recv_status::would_block)
        co_return;
    // naive method/path parse
    std::string_view req(span.data(), span.size());
    bool metrics = req.rfind("GET /metrics", 0) == 0;
    std::string body = metrics ? build_metrics_body() : std::string("not found\n");
    std::ostringstream resp;
    resp << "HTTP/1.1 " << (metrics ? "200 OK" : "404 Not Found") << "\r\n";
    resp << "Content-Type: text/plain; version=0.0.4\r\n";
    resp << "Content-Length: " << body.size() << "\r\n";
    resp << "Connection: close\r\n\r\n";
    resp << body;
    auto s = resp.str();
    std::span<const char> out{s.data(), s.size()};
    while (!out.empty()) {
        co_await client.poll(coro::poll_op::write);
        auto [st, rest] = client.send(out);
        if (st == coro::net::send_status::ok || st == coro::net::send_status::would_block) {
            out = rest;
            continue;
        }
        break;
    }
    co_return;
}

coro::task<void> run_metrics_endpoint(std::shared_ptr<coro::io_scheduler> scheduler, uint16_t port)
{
    co_await scheduler->schedule();
    t2d::log::info("[metrics] HTTP endpoint on port {}", port);
    coro::net::tcp::server server{scheduler, coro::net::tcp::server::options{.port = port}};
    while (true) {
        auto st = co_await server.poll();
        if (st == coro::poll_status::event) {
            auto client = server.accept();
            if (client.socket().is_valid()) {
                scheduler->spawn(handle_client(scheduler, std::move(client)));
            }
        } else if (st == coro::poll_status::error || st == coro::poll_status::closed) {
            t2d::log::error("[metrics] server poll error/closed");
            co_return;
        }
    }
}

} // namespace t2d::net
