#include "drv8835.hpp"

#include <pthread.h>
#include <sched.h>

#include <algorithm>
#include <cstdlib>

#include "log.hpp"
#include "rumicar/logic_api.h"

namespace rumicar::hw {

namespace {
constexpr int kRealtimePriority = 10;   // as the steering PWM: above every normal thread only
}

Drv8835::Drv8835(SoftPwmPair::Write steer_write, int steer_frequency_hz, const std::string& pwm_chip, int pwm_frequency_hz,
                 Pins pins, Kick kick)
    : pins_(pins), kick_(kick), steer_(std::move(steer_write), steer_frequency_hz) {
    in1_ = std::make_unique<PwmChannel>(pwm_chip, pins_.drive_in1, pwm_frequency_hz);
    in2_ = std::make_unique<PwmChannel>(pwm_chip, pins_.drive_in2, pwm_frequency_hz);
    kick_thread_ = std::thread([this] { kick_worker(); });
}

std::pair<int, int> Drv8835::steer_inputs(uint8_t steer, uint8_t steer_pwm) {
    const int s = steer_pwm;
    if (s == 0) return {0, 0};
    if (steer == RUMI_STEER_RIGHT) return {RUMI_PWM_MAX, RUMI_PWM_MAX - s};
    if (steer == RUMI_STEER_LEFT) return {RUMI_PWM_MAX - s, RUMI_PWM_MAX};
    return {0, 0};   // CENTER (and anything else): neutral
}

Drv8835::~Drv8835() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        quit_ = true;
    }
    kick_wake_.notify_all();
    kick_thread_.join();
    try {
        stop();
        // Leave the drive inputs low (coast) so the bridge is idle after exit.
        in1_->set_duty(0);
        in2_->set_duty(0);
    } catch (...) {
    }
}

void Drv8835::set_drive_locked(Dir dir, int pwm) {
    const int s = std::clamp(pwm, 0, RUMI_PWM_MAX);
    switch (dir) {
    case Dir::Coast:
        in1_->set_duty(0);
        in2_->set_duty(0);
        break;
    case Dir::Brake:
        in1_->set_duty(RUMI_PWM_MAX);
        in2_->set_duty(RUMI_PWM_MAX);
        break;
    case Dir::Forward:
        // Slow decay (RC_drive): IN1 held high, IN2 carries the inverted duty
        // (low = drive, high = brake); s = 255 is continuous drive.
        in1_->set_duty(RUMI_PWM_MAX);
        in2_->set_duty(RUMI_PWM_MAX - s);
        break;
    case Dir::Reverse:
        in2_->set_duty(RUMI_PWM_MAX);
        in1_->set_duty(RUMI_PWM_MAX - s);
        break;
    }
}

void Drv8835::apply(uint8_t steer, uint8_t steer_pwm, uint8_t drive_dir, uint8_t drive_pwm, int kick_cap) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Steering (RC_steer with its strength).
    const auto [a1, a2] = steer_inputs(steer, steer_pwm);
    steer_.set(a1, a2);

    Dir dir;
    int pwm = 0;
    switch (drive_dir) {
    case RUMI_DRIVE_FORWARD: dir = Dir::Forward; pwm = drive_pwm; break;
    case RUMI_DRIVE_REVERSE: dir = Dir::Reverse; pwm = drive_pwm; break;
    case RUMI_DRIVE_BRAKE:   dir = Dir::Brake; break;
    default:                 dir = Dir::Coast; break;
    }

    // Start kick: when motion starts from rest or reverses direction, raise
    // the duty to the kick (capped at the mode's limit) for kick_.duration_ms
    // to overcome static friction (RC_drive).  The worker ends it on time.
    const int kick = std::clamp(std::min(kick_.pwm, kick_cap), 0, RUMI_PWM_MAX);
    const bool moving = dir == Dir::Forward || dir == Dir::Reverse;
    const auto now = Clock::now();
    bool started = false;
    if (kicking_ && (!moving || dir != last_dir_ || now >= kick_until_)) {
        kicking_ = false;   // stopped, reversed, or over (the worker may not have run yet)
        ++kick_generation_;
    }
    if (moving && pwm > 0 && dir != last_dir_ && kick_.duration_ms > 0 && kick > 0) {
        kicking_ = true;
        kick_until_ = now + std::chrono::milliseconds(kick_.duration_ms);
        ++kick_generation_;
        started = true;
    }

    set_drive_locked(dir, kicking_ ? std::max(pwm, kick) : pwm);
    last_dir_ = dir;
    last_pwm_ = pwm;
    if (started) kick_wake_.notify_all();
}

void Drv8835::kick_worker() {
    std::unique_lock<std::mutex> lock(mutex_);
    bool started = false;
    while (!quit_) {
        if (!kicking_) {
            kick_wake_.wait(lock, [&] { return quit_ || kicking_; });
            continue;
        }
        if (!started) {
            started = true;
            sched_param sp{};
            sp.sched_priority = kRealtimePriority;
            kick_realtime_ = ::pthread_setschedparam(::pthread_self(), SCHED_FIFO, &sp) == 0;
            log::info(std::string("start kick: ") + std::to_string(kick_.duration_ms) + " ms, " +
                      (kick_realtime_ ? "SCHED_FIFO" : "normal scheduling (SCHED_FIFO not permitted)"));
        }
        const uint64_t gen = kick_generation_;
        const auto until = kick_until_;
        if (kick_wake_.wait_until(lock, until, [&] { return quit_ || !kicking_ || kick_generation_ != gen; })) continue;
        // The kick is over: back to the duty commanded last.
        kicking_ = false;
        try {
            set_drive_locked(last_dir_, last_pwm_);
        } catch (const std::exception& e) {
            log::error(std::string("start kick: cannot end the kick: ") + e.what());
        }
    }
}

bool Drv8835::kick_realtime() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return kick_realtime_;
}

void Drv8835::stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    steer_.set(0, 0);
    set_drive_locked(Dir::Brake, 0);
    last_dir_ = Dir::Brake;
    last_pwm_ = 0;
    if (kicking_) {
        kicking_ = false;
        ++kick_generation_;
    }
    kick_wake_.notify_all();
}

} // namespace rumicar::hw
