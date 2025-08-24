// SPDX-License-Identifier: Apache-2.0
// Asynchronous structured logger (header-only) inspired by udp2tcp logger.
// Simplified to use a dedicated background thread instead of coroutine queue
// to avoid dependency on coroutine mutex/queue types that are not enabled in
// current libcoro build. Provides:
//  - Level filtering via T2D_LOG_LEVEL (debug|info|warn|error)
//  - JSON mode via T2D_LOG_JSON presence
//  - Non-blocking enqueue (bounded by memory) with fallback synchronous path
//  - Optional external callback (set_callback)

#pragma once

#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>

namespace t2d::log {

enum class level
{
    debug = 0,
    info = 1,
    warn = 2,
    error = 3
};

namespace detail {
struct item
{
    level lv;
    std::string msg;
    std::chrono::system_clock::time_point ts;
};

inline std::atomic<int> g_level{static_cast<int>(level::info)};
inline std::atomic<bool> g_json{false};
inline std::atomic<bool> g_started{false};
inline std::atomic<bool> g_running{false};
inline std::atomic<bool> g_app_id_enabled{false};
inline std::string g_app_id; // guarded by g_io_mtx when modified/read for output
inline std::mutex g_q_mtx;
inline std::condition_variable g_q_cv;
inline std::deque<item> g_queue;
inline std::mutex g_io_mtx;
using cb_sig = void (*)(int, const char *, void *);
inline std::atomic<void *> g_cb_ptr{nullptr};
inline std::atomic<void *> g_cb_ud{nullptr};

inline cb_sig load_cb()
{
    return reinterpret_cast<cb_sig>(g_cb_ptr.load(std::memory_order_acquire));
}

inline const char *level_name(level lv)
{
    switch (lv) {
        case level::debug:
            return "debug";
        case level::info:
            return "info";
        case level::warn:
            return "warn";
        case level::error:
            return "error";
    }
    return "info";
}

inline int parse_level(const std::string &s)
{
    std::string v;
    v.reserve(s.size());
    for (char c : s)
        v.push_back(std::tolower(static_cast<unsigned char>(c)));
    if (v == "debug")
        return (int)level::debug;
    if (v == "info")
        return (int)level::info;
    if (v == "warn" || v == "warning")
        return (int)level::warn;
    if (v == "error" || v == "err")
        return (int)level::error;
    return (int)level::info;
}
} // namespace detail

// Formatting helpers are outside detail to be visible to variadic logging wrappers.
namespace detail_format {
template <typename T>
inline std::string to_string_any(const T &v)
{
    if constexpr (std::is_same_v<std::decay_t<T>, std::string>)
        return v;
    else if constexpr (std::is_same_v<std::decay_t<T>, const char *>)
        return v ? std::string(v) : std::string();
    else if constexpr (std::is_convertible_v<T, std::string_view>)
        return std::string(std::string_view(v));
    else if constexpr (std::is_arithmetic_v<T>) {
        if constexpr (std::is_floating_point_v<T>) {
            std::ostringstream oss;
            oss.setf(std::ios::fixed, std::ios::floatfield);
            oss << v;
            return oss.str();
        } else
            return std::to_string(v);
    } else {
        std::ostringstream oss;
        oss << v;
        return oss.str();
    }
}

inline void append_remaining_args(std::string &) {}

template <typename First, typename... Rest>
inline void append_remaining_args(std::string &out, First &&f, Rest &&...rest)
{
    out.push_back(' ');
    out += to_string_any(std::forward<First>(f));
    if constexpr (sizeof...(Rest) > 0)
        append_remaining_args(out, std::forward<Rest>(rest)...);
}

template <typename... Args>
inline std::string tiny_format(std::string_view fmt, Args &&...args)
{
    if constexpr (sizeof...(Args) == 0)
        return std::string(fmt);
    constexpr size_t N = sizeof...(Args);
    std::array<std::string, N> values{to_string_any(std::forward<Args>(args))...};
    std::string out;
    out.reserve(fmt.size() + N * 8);
    size_t search_pos = 0;
    size_t idx = 0;
    while (idx < N) {
        size_t p = fmt.find("{}", search_pos);
        if (p == std::string_view::npos)
            break; // no more placeholders
        out.append(fmt.substr(search_pos, p - search_pos));
        out += values[idx++];
        search_pos = p + 2;
    }
    // append rest of format string (includes any unmatched placeholders)
    out.append(fmt.substr(search_pos));
    // append remaining arg values (if any) space-separated
    if (idx < N) {
        for (; idx < N; ++idx) {
            out.push_back(' ');
            out += values[idx];
        }
    }
    return out;
}
} // namespace detail_format

namespace detail {
inline void format_and_write(level lv, const std::string &m, std::chrono::system_clock::time_point tp)
{
    std::time_t tt = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    if (g_json.load(std::memory_order_relaxed)) {
        std::lock_guard lk(g_io_mtx);
        std::cerr << '{' << "\"ts\":\"" << std::put_time(&tm, "%Y-%m-%dT%H:%M:%S") << "\",\"level\":\""
                  << level_name(lv) << "\",\"msg\":\"";
        for (char c : m) {
            if (c == '"')
                std::cerr << "\\\"";
            else
                std::cerr << c;
        }
        std::cerr << "\"}" << std::endl;
    } else {
        std::lock_guard lk(g_io_mtx);
        char buf[16];
        std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
        const char *tag = lv == level::debug ? "D" : lv == level::info ? "I" : lv == level::warn ? "W" : "E";
        if (g_app_id_enabled.load(std::memory_order_relaxed) && !g_app_id.empty()) {
            std::cerr << g_app_id << ' ';
        }
        std::cerr << '[' << tag << ' ' << buf << "] " << m << std::endl;
    }
    if (auto cb = load_cb())
        cb((int)lv, m.c_str(), g_cb_ud.load(std::memory_order_relaxed));
}

inline std::thread g_thread; // background consumer

inline void consumer_thread()
{
    while (g_running.load(std::memory_order_acquire)) {
        std::deque<item> local;
        {
            std::unique_lock lk(g_q_mtx);
            g_q_cv.wait(lk, [] { return !g_running.load(std::memory_order_acquire) || !g_queue.empty(); });
            if (!g_running.load(std::memory_order_acquire) && g_queue.empty())
                break;
            local.swap(g_queue);
        }
        for (auto &it : local)
            format_and_write(it.lv, it.msg, it.ts);
    }
    // flush any remaining messages (defensive)
    std::deque<item> rest;
    {
        std::lock_guard lk(g_q_mtx);
        rest.swap(g_queue);
    }
    for (auto &it : rest)
        format_and_write(it.lv, it.msg, it.ts);
}

inline void shutdown()
{
    if (!g_started.load(std::memory_order_acquire))
        return;
    if (!g_running.exchange(false, std::memory_order_acq_rel))
        return;
    g_q_cv.notify_all();
    if (g_thread.joinable())
        g_thread.join();
}

inline void start()
{
    if (g_started.load(std::memory_order_acquire))
        return;
    if (const char *lvl = std::getenv("T2D_LOG_LEVEL"))
        g_level.store(parse_level(lvl), std::memory_order_relaxed);
    if (std::getenv("T2D_LOG_JSON"))
        g_json.store(true, std::memory_order_relaxed);
    if (const char *app = std::getenv("T2D_LOG_APP_ID")) {
        if (*app) {
            std::lock_guard lk(g_io_mtx);
            g_app_id.assign(app);
            g_app_id_enabled.store(true, std::memory_order_relaxed);
        }
    }
    if (!g_started.exchange(true, std::memory_order_acq_rel)) {
        g_running.store(true, std::memory_order_release);
        g_thread = std::thread([] { consumer_thread(); });
        std::atexit([] { shutdown(); });
    }
}
} // namespace detail

inline void init()
{
    detail::start();
}

inline void set_app_id(std::string id)
{
    std::lock_guard lk(detail::g_io_mtx);
    detail::g_app_id = std::move(id);
    detail::g_app_id_enabled.store(true, std::memory_order_relaxed);
}

inline void disable_app_id()
{
    detail::g_app_id_enabled.store(false, std::memory_order_relaxed);
}

inline bool enabled(level lv) noexcept
{
    return (int)lv >= detail::g_level.load(std::memory_order_relaxed);
}

inline void set_callback(void (*cb)(int, const char *, void *), void *ud) noexcept
{
    detail::g_cb_ptr.store(reinterpret_cast<void *>(cb), std::memory_order_release);
    detail::g_cb_ud.store(ud, std::memory_order_release);
}

inline void write(level lv, std::string_view msg)
{
    if (!enabled(lv))
        return;
    detail::start();
    auto tp = std::chrono::system_clock::now();
    if (detail::g_started.load(std::memory_order_acquire)) {
        {
            std::lock_guard lk(detail::g_q_mtx);
            detail::g_queue.push_back(detail::item{lv, std::string(msg), tp});
        }
        detail::g_q_cv.notify_one();
    } else {
        detail::format_and_write(lv, std::string(msg), tp);
    }
}

inline void debug(std::string_view m)
{
    write(level::debug, m);
}

inline void info(std::string_view m)
{
    write(level::info, m);
}

inline void warn(std::string_view m)
{
    write(level::warn, m);
}

inline void error(std::string_view m)
{
    write(level::error, m);
}

// Variadic formatting convenience wrappers ({} placeholder based)
template <typename... Args>
inline void debug(const char *fmt, Args &&...args)
{
    if (enabled(level::debug))
        write(level::debug, detail_format::tiny_format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
inline void info(const char *fmt, Args &&...args)
{
    if (enabled(level::info))
        write(level::info, detail_format::tiny_format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
inline void warn(const char *fmt, Args &&...args)
{
    if (enabled(level::warn))
        write(level::warn, detail_format::tiny_format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
inline void error(const char *fmt, Args &&...args)
{
    if (enabled(level::error))
        write(level::error, detail_format::tiny_format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
inline void write(level lv, const char *fmt, Args &&...args)
{
    if (enabled(lv))
        write(lv, detail_format::tiny_format(fmt, std::forward<Args>(args)...));
}

} // namespace t2d::log
