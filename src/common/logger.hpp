#pragma once
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string_view>

namespace t2d::log {
inline std::string ts()
{
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[32];
    strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
    return buf;
}

enum class level
{
    debug = 0,
    info = 1,
    warn = 2,
    error = 3
};

inline level current_level()
{
    static level lvl = []()
    {
        if (const char *env = std::getenv("T2D_LOG_LEVEL")) {
            std::string v(env);
            if (v == "debug")
                return level::debug;
            if (v == "info")
                return level::info;
            if (v == "warn")
                return level::warn;
            if (v == "error")
                return level::error;
        }
        return level::info;
    }();
    return lvl;
}

inline bool json_mode()
{
    static bool jm = []()
    {
        return std::getenv("T2D_LOG_JSON") != nullptr;
    }();
    return jm;
}

inline const char *level_str(level l)
{
    switch (l) {
        case level::debug:
            return "debug";
        case level::info:
            return "info";
        case level::warn:
            return "warn";
        default:
            return "error";
    }
}

inline void log(level l, std::string_view m)
{
    if (static_cast<int>(l) < static_cast<int>(current_level()))
        return;
    auto &out = (l == level::error) ? std::cerr : std::cout;
    if (json_mode()) {
        out << "{\"ts\":\"" << ts() << "\",\"level\":\"" << level_str(l) << "\",\"msg\":\"";
        for (char c : m) {
            if (c == '"')
                out << '\\' << '"';
            else
                out << c;
        }
        out << "\"}" << '\n';
    } else {
        const char *tag = l == level::debug ? "D" : l == level::info ? "I" : l == level::warn ? "W" : "E";
        out << '[' << tag << ' ' << ts() << "] " << m << '\n';
    }
}

inline void debug(std::string_view m)
{
    log(level::debug, m);
}

inline void info(std::string_view m)
{
    log(level::info, m);
}

inline void warn(std::string_view m)
{
    log(level::warn, m);
}

inline void error(std::string_view m)
{
    log(level::error, m);
}
}
