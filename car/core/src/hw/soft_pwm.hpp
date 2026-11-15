// Software PWM on two GPIO output lines: the steering inputs AIN1/AIN2 of the
// DRV8835, which are on GPIO 17/27 (no hardware PWM there; the two hardware
// PWM channels of the Zero 2 W drive the B bridge).  RC_steer's strength is
// produced as in RasPi/RumiCar.py (pigpio software PWM, 500 Hz, duty 0..255).
//
// Each line has a duty 0..255 (the share of the period it is high).  A line
// at 0 or 255 is static: it is written once, at once, by set() on the
// caller's thread, so CENTER and full steering behave exactly as plain GPIO
// writes.  Only while a line has a duty of 1..254 does a worker thread run,
// raising the PWM lines at the start of each period and lowering each at its
// duty; with no such line it sleeps without waking.  A change of the duties
// takes effect at once: the worker drops the period under way and starts a
// new one with the new values (no stale edge is written after set()
// returns).  The worker asks for SCHED_FIFO so that the edges keep their
// timing while the camera and the control loop are busy; without it the
// output is the same, with more jitter.
#pragma once

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace rumicar::hw {

class SoftPwmPair {
public:
    // write(index, level) drives line 0 or 1 (it may throw std::runtime_error).
    using Write = std::function<void(unsigned index, bool level)>;

    static constexpr int kMax = 255;

    // Both lines are written low.  frequency_hz: 50..2000.
    SoftPwmPair(Write write, int frequency_hz);
    // Stops the worker and leaves both lines low.
    ~SoftPwmPair();
    SoftPwmPair(const SoftPwmPair&) = delete;
    SoftPwmPair& operator=(const SoftPwmPair&) = delete;

    // Duties 0..255 (clamped).  Static lines are written before this returns;
    // a write error of those is thrown (std::runtime_error).  Thread-safe.
    void set(int duty0, int duty1);

    std::array<int, 2> duties() const;
    std::chrono::nanoseconds period() const { return period_; }

    // Write errors of the worker so far (each is also logged, the first few in
    // full); the worker keeps running after an error.
    uint64_t worker_errors() const;
    // Whether the worker runs with SCHED_FIFO (false until it first starts).
    bool realtime() const;

private:
    void run();
    bool pwm_active_locked() const;

    Write write_;
    std::chrono::nanoseconds period_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::array<int, 2> duty_{0, 0};
    std::array<int, 2> level_{0, 0};   // last level written: 0 low, 1 high
    uint64_t generation_ = 0;          // incremented by every set()
    uint64_t worker_errors_ = 0;
    bool realtime_ = false;
    bool quit_ = false;
    std::thread worker_;
};

} // namespace rumicar::hw
