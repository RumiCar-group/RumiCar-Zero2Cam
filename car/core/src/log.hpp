// Minimal timestamped logger (stderr -> journald under systemd) that also
// keeps the most recent lines in memory so the PC can fetch them over the
// control channel (get_log) without a journal client on the car.
#pragma once

#include <chrono>
#include <cstdio>
#include <ctime>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace rumicar::log {

enum class Level { Info, Warn, Error };

constexpr size_t kRecentLines = 500;

namespace detail {
inline std::mutex& mutex() { static std::mutex m; return m; }
inline std::deque<std::string>& recent() { static std::deque<std::string> d; return d; }
} // namespace detail

inline void write(Level level, const std::string& msg) {
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    char ts[40];
    std::tm tm{};
    gmtime_r(&t, &tm);
    std::strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%S", &tm);
    const char* tag = level == Level::Info ? "INFO" : level == Level::Warn ? "WARN" : "ERROR";
    char head[64];
    std::snprintf(head, sizeof head, "%s.%03lldZ %s ", ts, static_cast<long long>(ms), tag);
    const std::string line = std::string(head) + msg;

    std::lock_guard<std::mutex> lock(detail::mutex());
    std::fprintf(stderr, "%s\n", line.c_str());
    std::fflush(stderr);
    auto& d = detail::recent();
    d.push_back(line);
    while (d.size() > kRecentLines) d.pop_front();
}

inline void info(const std::string& m) { write(Level::Info, m); }
inline void warn(const std::string& m) { write(Level::Warn, m); }
inline void error(const std::string& m) { write(Level::Error, m); }

// The last `n` lines (oldest first), at most kRecentLines.
inline std::vector<std::string> recent(size_t n) {
    std::lock_guard<std::mutex> lock(detail::mutex());
    const auto& d = detail::recent();
    const size_t take = n < d.size() ? n : d.size();
    return std::vector<std::string>(d.end() - static_cast<std::ptrdiff_t>(take), d.end());
}

} // namespace rumicar::log
