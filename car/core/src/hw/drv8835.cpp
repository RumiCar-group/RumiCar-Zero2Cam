#include "drv8835.hpp"

#include <algorithm>
#include <cstdlib>

#include "rumicar/logic_api.h"

namespace rumicar::hw {

Drv8835::Drv8835(GpioOutputs& gpio, int steer_frequency_hz, const std::string& pwm_chip, int pwm_frequency_hz, Pins pins,
                 Kick kick)
    : pins_(pins),
      kick_(kick),
      steer_([&gpio, pins](unsigned line, bool level) { gpio.set(line == 0 ? pins.steer_in1 : pins.steer_in2, level); },
             steer_frequency_hz) {
    in1_ = std::make_unique<PwmChannel>(pwm_chip, pins_.drive_in1, pwm_frequency_hz);
    in2_ = std::make_unique<PwmChannel>(pwm_chip, pins_.drive_in2, pwm_frequency_hz);
}

std::pair<int, int> Drv8835::steer_inputs(uint8_t steer, uint8_t steer_pwm) {
    const int s = steer_pwm;
    if (s == 0) return {0, 0};
    if (steer == RUMI_STEER_RIGHT) return {RUMI_PWM_MAX, RUMI_PWM_MAX - s};
    if (steer == RUMI_STEER_LEFT) return {RUMI_PWM_MAX - s, RUMI_PWM_MAX};
    return {0, 0};   // CENTER (and anything else): neutral
}

Drv8835::~Drv8835() {
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

void Drv8835::apply(uint8_t steer, uint8_t steer_pwm, uint8_t drive_dir, uint8_t drive_pwm,
                    std::chrono::steady_clock::time_point now) {
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

    // Start kick: when motion starts from rest or reverses direction, apply a
    // strong pulse for kick_.duration_ms to overcome static friction (RC_drive).
    const bool moving = dir == Dir::Forward || dir == Dir::Reverse;
    if (moving && pwm > 0 && dir != last_dir_ && kick_.duration_ms > 0 && kick_.pwm > 0) {
        kicking_ = true;
        kick_until_ = now + std::chrono::milliseconds(kick_.duration_ms);
    }
    if (kicking_) {
        if (!moving || now >= kick_until_) kicking_ = false;
        else pwm = std::max(pwm, kick_.pwm);
    }

    set_drive_locked(dir, pwm);
    last_dir_ = dir;
}

void Drv8835::stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    steer_.set(0, 0);
    set_drive_locked(Dir::Brake, 0);
    last_dir_ = Dir::Brake;
    kicking_ = false;
}

} // namespace rumicar::hw
