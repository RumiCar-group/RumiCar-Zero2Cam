// Tests of the steering output: the duties RC_steer's strength gives on AIN1 /
// AIN2 (Drv8835::steer_inputs, the table of RumiCar-lib v0.6.0) and the
// software PWM that produces them (SoftPwmPair), with its real worker thread
// and timing.  The output function records every level written with its time,
// as the GPIO lines would see it.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "check.hpp"
#include "hw/drv8835.hpp"
#include "hw/soft_pwm.hpp"
#include "rumicar/logic_api.h"

using rumicar::hw::Drv8835;
using rumicar::hw::SoftPwmPair;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

namespace {

struct Event {
    Clock::time_point t;
    unsigned line;
    bool level;
};

class Recorder {
public:
    SoftPwmPair::Write output() {
        return [this](unsigned line, bool level) {
            if (fail_line_ >= 0 && static_cast<int>(line) == fail_line_ && level) throw std::runtime_error("line " + std::to_string(line) + " failed");
            std::lock_guard<std::mutex> lock(m_);
            events_.push_back({Clock::now(), line, level});
        };
    }
    std::vector<Event> events() const {
        std::lock_guard<std::mutex> lock(m_);
        return events_;
    }
    size_t count() const {
        std::lock_guard<std::mutex> lock(m_);
        return events_.size();
    }
    // Level of a line after all events so far (false before any).
    bool level(unsigned line) const {
        std::lock_guard<std::mutex> lock(m_);
        for (auto it = events_.rbegin(); it != events_.rend(); ++it)
            if (it->line == line) return it->level;
        return false;
    }
    void fail_high_writes_of(int line) { fail_line_ = line; }

private:
    mutable std::mutex m_;
    std::vector<Event> events_;
    std::atomic<int> fail_line_{-1};
};

struct Measured {
    double duty = 0;     // share of the window the line was high
    int rising = 0;      // rising edges in the window
};

// Time-weighted high share of `line` between `from` and `to`.
Measured measure(const std::vector<Event>& ev, unsigned line, Clock::time_point from, Clock::time_point to) {
    bool level = false;
    for (const Event& e : ev) {
        if (e.t > from) break;
        if (e.line == line) level = e.level;
    }
    Measured m;
    auto t = from;
    std::chrono::nanoseconds high{0};
    for (const Event& e : ev) {
        if (e.line != line || e.t <= from) continue;
        if (e.t >= to) break;
        if (level) high += e.t - t;
        if (!level && e.level) ++m.rising;
        level = e.level;
        t = e.t;
    }
    if (level) high += to - t;
    m.duty = std::chrono::duration<double>(high).count() / std::chrono::duration<double>(to - from).count();
    return m;
}

void test_steer_inputs_table() {
    // RC_steer(direc, s) of RumiCar-lib v0.6.0: RIGHT -> AIN1 255, AIN2 255 - s;
    // LEFT -> AIN1 255 - s, AIN2 255; CENTER or s = 0 -> both 0.
    using P = std::pair<int, int>;
    CHECK(Drv8835::steer_inputs(RUMI_STEER_RIGHT, 255) == P(255, 0));     // full: as the on/off steering up to ABI 3
    CHECK(Drv8835::steer_inputs(RUMI_STEER_LEFT, 255) == P(0, 255));
    CHECK(Drv8835::steer_inputs(RUMI_STEER_RIGHT, 128) == P(255, 127));
    CHECK(Drv8835::steer_inputs(RUMI_STEER_LEFT, 128) == P(127, 255));
    CHECK(Drv8835::steer_inputs(RUMI_STEER_RIGHT, 1) == P(255, 254));
    CHECK(Drv8835::steer_inputs(RUMI_STEER_LEFT, 254) == P(1, 255));
    CHECK(Drv8835::steer_inputs(RUMI_STEER_RIGHT, 0) == P(0, 0));         // 0 = neutral
    CHECK(Drv8835::steer_inputs(RUMI_STEER_LEFT, 0) == P(0, 0));
    CHECK(Drv8835::steer_inputs(RUMI_STEER_CENTER, 0) == P(0, 0));
    CHECK(Drv8835::steer_inputs(RUMI_STEER_CENTER, 200) == P(0, 0));      // CENTER ignores the strength
    CHECK(Drv8835::steer_inputs(7, 200) == P(0, 0));                      // unknown direction: neutral
}

void test_static_levels_are_written_at_once() {
    Recorder rec;
    {
        SoftPwmPair pwm(rec.output(), 500);
        CHECK(rec.count() == 2 && !rec.level(0) && !rec.level(1));   // both low from the start
        pwm.set(255, 0);                                             // RIGHT, full
        CHECK(rec.level(0) && !rec.level(1));
        const size_t n = rec.count();
        std::this_thread::sleep_for(30ms);
        CHECK_MSG(rec.count() == n, "a static output must not be rewritten (no worker activity)");
        pwm.set(0, 255);                                             // LEFT, full
        CHECK(!rec.level(0) && rec.level(1));
        pwm.set(0, 0);                                               // CENTER
        CHECK(!rec.level(0) && !rec.level(1));
        CHECK(pwm.duties() == (std::array<int, 2>{0, 0}));
        pwm.set(300, -4);                                            // clamped
        CHECK(pwm.duties() == (std::array<int, 2>{255, 0}));
        CHECK(rec.level(0) && !rec.level(1));
    }
    CHECK(!rec.level(0) && !rec.level(1));   // left low by the destructor
}

void test_duty_and_frequency() {
    Recorder rec;
    SoftPwmPair pwm(rec.output(), 500);
    CHECK(pwm.period() == 2ms);
    pwm.set(255, 127);   // RIGHT with strength 128: AIN1 high, AIN2 high 127/255 of each period
    std::this_thread::sleep_for(20ms);
    const auto from = Clock::now();
    std::this_thread::sleep_for(400ms);
    const auto to = Clock::now();
    const auto ev = rec.events();
    const Measured a1 = measure(ev, 0, from, to), a2 = measure(ev, 1, from, to);
    std::printf("soft PWM 500 Hz, AIN2 duty 127/255 = %.3f: measured %.3f, %d periods in 0.4 s (%s)\n", 127.0 / 255.0, a2.duty,
                a2.rising, pwm.realtime() ? "SCHED_FIFO" : "normal scheduling");
    CHECK_MSG(a1.duty > 0.999 && a1.rising == 0, "AIN1 must stay high");
    CHECK_MSG(a2.duty > 127.0 / 255.0 - 0.08 && a2.duty < 127.0 / 255.0 + 0.08, std::to_string(a2.duty));
    CHECK_MSG(a2.rising >= 160 && a2.rising <= 205, std::to_string(a2.rising) + " periods (200 expected)");

    // A new strength takes effect at once: LEFT with strength 215 -> AIN1 40/255, AIN2 high.
    pwm.set(40, 255);
    std::this_thread::sleep_for(10ms);
    const auto from2 = Clock::now();
    std::this_thread::sleep_for(400ms);
    const auto to2 = Clock::now();
    const auto ev2 = rec.events();
    const Measured b1 = measure(ev2, 0, from2, to2), b2 = measure(ev2, 1, from2, to2);
    std::printf("soft PWM 500 Hz, AIN1 duty 40/255 = %.3f: measured %.3f, %d periods\n", 40.0 / 255.0, b1.duty, b1.rising);
    CHECK_MSG(b2.duty > 0.999 && b2.rising == 0, "AIN2 must stay high");
    CHECK_MSG(b1.duty > 40.0 / 255.0 - 0.06 && b1.duty < 40.0 / 255.0 + 0.08, std::to_string(b1.duty));
    CHECK_MSG(b1.rising >= 160 && b1.rising <= 205, std::to_string(b1.rising) + " periods (200 expected)");

    // Back to CENTER: both low when set() returns, and the worker writes nothing afterwards.
    pwm.set(0, 0);
    CHECK(!rec.level(0) && !rec.level(1));
    const size_t n = rec.count();
    std::this_thread::sleep_for(30ms);
    CHECK_MSG(rec.count() == n, "no edge may follow set() of a static output");
    CHECK(pwm.worker_errors() == 0);
}

void test_rapid_changes_never_leave_a_stale_level() {
    // The control loop changes the strength up to every 50 ms; here much faster.
    // After each set() of a static output the lines are at that output at once.
    Recorder rec;
    SoftPwmPair pwm(rec.output(), 500);
    for (int i = 0; i < 200; ++i) {
        pwm.set(255, (i * 37) % 254 + 1);
        std::this_thread::sleep_for(std::chrono::microseconds(300 + (i % 7) * 211));
        if (i % 10 == 9) {
            pwm.set(0, 255);
            CHECK(!rec.level(0) && rec.level(1));
        }
    }
    pwm.set(0, 0);
    CHECK(!rec.level(0) && !rec.level(1));
    const size_t n = rec.count();
    std::this_thread::sleep_for(20ms);
    CHECK(rec.count() == n);
}

void test_write_errors() {
    // A failing static write is reported to the caller (the control loop logs
    // "motor output failed"); the worker counts its failures and keeps going.
    Recorder rec;
    SoftPwmPair pwm(rec.output(), 1000);
    rec.fail_high_writes_of(0);
    bool threw = false;
    try { pwm.set(255, 0); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);
    pwm.set(0, 0);
    rec.fail_high_writes_of(1);
    pwm.set(255, 100);   // AIN1 static high (fine), AIN2 PWM: its rising edges fail in the worker
    std::this_thread::sleep_for(50ms);
    CHECK(pwm.worker_errors() > 10);
    CHECK(rec.level(0));
    rec.fail_high_writes_of(-1);
    std::this_thread::sleep_for(20ms);
    const auto from = Clock::now();
    std::this_thread::sleep_for(100ms);
    const Measured m = measure(rec.events(), 1, from, Clock::now());
    CHECK_MSG(m.rising >= 70, "the worker resumes after errors: " + std::to_string(m.rising) + " periods in 0.1 s at 1 kHz");
    pwm.set(0, 0);
}

void test_bad_arguments() {
    Recorder rec;
    bool threw = false;
    try { SoftPwmPair x(rec.output(), 49); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);
    threw = false;
    try { SoftPwmPair x(rec.output(), 2001); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);
    threw = false;
    try { SoftPwmPair x(nullptr, 500); } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw);
}

} // namespace

int main() {
    test_steer_inputs_table();
    test_static_levels_are_written_at_once();
    test_duty_and_frequency();
    test_rapid_changes_never_leave_a_stale_level();
    test_write_errors();
    test_bad_arguments();
    return check::summary("test_soft_pwm");
}
