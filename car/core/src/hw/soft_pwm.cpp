#include "soft_pwm.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include <pthread.h>
#include <sched.h>

#include "log.hpp"

namespace rumicar::hw {

namespace {

using Clock = std::chrono::steady_clock;

constexpr int kRealtimePriority = 10;     // low among SCHED_FIFO priorities (1..99): above every normal thread only
constexpr uint64_t kLoggedErrors = 5;     // worker write errors logged in full; later ones are only counted

} // namespace

SoftPwmPair::SoftPwmPair(Write write, int frequency_hz) : write_(std::move(write)) {
    if (frequency_hz < 50 || frequency_hz > 2000) throw std::runtime_error("SoftPwmPair: frequency must be 50..2000 Hz");
    if (!write_) throw std::runtime_error("SoftPwmPair: no output");
    period_ = std::chrono::nanoseconds(1'000'000'000LL / frequency_hz);
    write_(0, false);
    write_(1, false);
    worker_ = std::thread([this] { run(); });
}

SoftPwmPair::~SoftPwmPair() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        quit_ = true;
    }
    wake_.notify_all();
    worker_.join();
    try {
        write_(0, false);
        write_(1, false);
    } catch (...) {
        // Destructors must not throw.
    }
}

bool SoftPwmPair::pwm_active_locked() const {
    return (duty_[0] > 0 && duty_[0] < kMax) || (duty_[1] > 0 && duty_[1] < kMax);
}

void SoftPwmPair::set(int duty0, int duty1) {
    std::lock_guard<std::mutex> lock(mutex_);
    duty_ = {std::clamp(duty0, 0, kMax), std::clamp(duty1, 0, kMax)};
    ++generation_;   // the worker drops the period under way
    for (unsigned i = 0; i < 2; ++i) {
        const int d = duty_[i];
        if (d != 0 && d != kMax) continue;   // a PWM line: the worker writes it
        const int level = d == kMax ? 1 : 0;
        if (level_[i] != level) {
            write_(i, level != 0);
            level_[i] = level;
        }
    }
    wake_.notify_all();
}

std::array<int, 2> SoftPwmPair::duties() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return duty_;
}

uint64_t SoftPwmPair::worker_errors() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return worker_errors_;
}

bool SoftPwmPair::realtime() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return realtime_;
}

void SoftPwmPair::run() {
    std::unique_lock<std::mutex> lock(mutex_);
    bool started = false;

    // Writes a level from the worker (with the mutex held); an error is counted
    // and logged, and the level is tried again in the next period.
    const auto put = [&](unsigned i, int level) {
        if (level_[i] == level) return;
        try {
            write_(i, level != 0);
            level_[i] = level;
        } catch (const std::exception& e) {
            if (++worker_errors_ <= kLoggedErrors)
                log::error(std::string("steering PWM: ") + e.what() + (worker_errors_ == kLoggedErrors ? " (further errors are not logged)" : ""));
        }
    };

    while (!quit_) {
        if (!pwm_active_locked()) {
            wake_.wait(lock, [&] { return quit_ || pwm_active_locked(); });
            continue;
        }
        if (!started) {
            started = true;
            sched_param sp{};
            sp.sched_priority = kRealtimePriority;
            realtime_ = ::pthread_setschedparam(::pthread_self(), SCHED_FIFO, &sp) == 0;
            log::info(std::string("steering PWM: ") + std::to_string(1'000'000'000LL / period_.count()) + " Hz, " +
                      (realtime_ ? "SCHED_FIFO" : "normal scheduling (SCHED_FIFO not permitted)"));
        }

        // One period with the duties of this generation: every line with a duty
        // above 0 goes high at the start, each PWM line low at its duty.
        const uint64_t gen = generation_;
        const std::array<int, 2> d = duty_;
        const auto start = Clock::now();
        put(0, d[0] > 0 ? 1 : 0);
        put(1, d[1] > 0 ? 1 : 0);
        struct Edge {
            std::chrono::nanoseconds at;
            unsigned line;
        };
        std::array<Edge, 2> falls{};
        size_t n = 0;
        for (unsigned i = 0; i < 2; ++i) {
            if (d[i] > 0 && d[i] < kMax) falls[n++] = {period_ * d[i] / kMax, i};
        }
        if (n == 2 && falls[1].at < falls[0].at) std::swap(falls[0], falls[1]);

        const auto changed = [&] { return quit_ || generation_ != gen; };
        bool restart = false;
        for (size_t k = 0; k < n; ++k) {
            const Edge& e = falls[k];
            if (wake_.wait_until(lock, start + e.at, changed)) {
                restart = true;
                break;
            }
            put(e.line, 0);
        }
        if (!restart) wake_.wait_until(lock, start + period_, changed);
    }
}

} // namespace rumicar::hw
