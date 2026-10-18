// Minimal test harness used by the car-side unit tests (no external dependency).
#pragma once

#include <cstdio>
#include <string>

namespace check {

inline int g_failures = 0;
inline int g_checks = 0;

inline void record(bool ok, const char* expr, const char* file, int line, const std::string& note) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::fprintf(stderr, "FAIL %s:%d: %s%s%s\n", file, line, expr, note.empty() ? "" : " -- ", note.c_str());
    }
}

inline int summary(const char* name) {
    std::printf("%s: %d checks, %d failures\n", name, g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}

} // namespace check

#define CHECK(expr) ::check::record(static_cast<bool>(expr), #expr, __FILE__, __LINE__, "")
#define CHECK_MSG(expr, note) ::check::record(static_cast<bool>(expr), #expr, __FILE__, __LINE__, (note))
