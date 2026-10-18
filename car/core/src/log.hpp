// Minimal timestamped logger (stderr -> journald under systemd).
#pragma once

#include <chrono>
#include <cstdio>
#include <ctime>
#include <string>

namespace rumicar::log {

enum class Level { Info, Warn, Error };

inline void write(Level level, const std::string& msg) {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    char ts[32];
    std::tm tm{};
    gmtime_r(&t, &tm);
    std::strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%S", &tm);
    const char* tag = level == Level::Info ? "INFO" : level == Level::Warn ? "WARN" : "ERROR";
    std::fprintf(stderr, "%s.%03lldZ %s %s\n", ts, static_cast<long long>(ms), tag, msg.c_str());
    std::fflush(stderr);
}

inline void info(const std::string& m) { write(Level::Info, m); }
inline void warn(const std::string& m) { write(Level::Warn, m); }
inline void error(const std::string& m) { write(Level::Error, m); }

} // namespace rumicar::log
