#pragma once
#include <iostream>
#include <chrono>
#include <iomanip>
#include <string_view>
#include <cstdlib>

namespace t2d::log {
inline std::string ts(){
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{}; gmtime_r(&t, &tm);
    char buf[32]; strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
    return buf;
}

enum class level { debug=0, info=1, warn=2, error=3 };

inline level current_level() {
    static level lvl = [](){
        if(const char* env = std::getenv("T2D_LOG_LEVEL")) {
            std::string v(env);
            if(v=="debug") return level::debug;
            if(v=="info") return level::info;
            if(v=="warn") return level::warn;
            if(v=="error") return level::error;
        }
        return level::info;
    }();
    return lvl;
}

inline void log(level l, std::string_view m) {
    if(static_cast<int>(l) < static_cast<int>(current_level())) return;
    const char* tag = l==level::debug?"D": l==level::info?"I": l==level::warn?"W":"E";
    auto &out = (l==level::error)? std::cerr : std::cout;
    out << '[' << tag << ' ' << ts() << "] " << m << '\n';
}
inline void debug(std::string_view m){ log(level::debug,m);} 
inline void info(std::string_view m){ log(level::info,m);} 
inline void warn(std::string_view m){ log(level::warn,m);} 
inline void error(std::string_view m){ log(level::error,m);} 
}
