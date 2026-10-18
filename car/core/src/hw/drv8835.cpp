#include "drv8835.hpp"

#include <algorithm>
#include <cstdlib>

namespace rumicar::hw {

Drv8835::Drv8835(GpioOutputs& gpio, const std::string& pwm_chip, int pwm_frequency_hz, Pins pins, Kick kick)
    : gpio_(gpio), pins_(pins), kick_(kick) {
    in1_ = std::make_unique<PwmChannel>(pwm_chip, pins_.drive_in1, pwm_frequency_hz);
    in2_ = std::make_unique<PwmChannel>(pwm_chip, pins_.drive_in2, pwm_frequency_hz);
    gpio_.set(pins_.steer_in1, false);
    gpio_.set(pins_.steer_in2, false);
}

Drv8835::~Drv8835() {
    try {
        stop();
        // Leave the drive inputs low (coast) so the bridge is idle after exit.
        in1_->set_duty_permille(0);
        in2_->set_duty_permille(0);
    } catch (...) {
    }
}

void Drv8835::set_drive_locked(Dir dir, int magnitude_permille) {
    const int s = std::clamp(magnitude_permille, 0, 1000);
    switch (dir) {
    case Dir::Coast:
        in1_->set_duty_permille(0);
        in2_->set_duty_permille(0);
        break;
    case Dir::Brake:
        in1_->set_duty_permille(1000);
        in2_->set_duty_permille(1000);
        break;
    case Dir::Forward:
        // Slow decay: IN1 held high, IN2 carries the inverted duty (low = drive, high = brake).
        in1_->set_duty_permille(1000);
        in2_->set_duty_permille(1000 - s);
        break;
    case Dir::Reverse:
        in2_->set_duty_permille(1000);
        in1_->set_duty_permille(1000 - s);
        break;
    }
}

void Drv8835::apply(int8_t steer, int drive_permille, bool brake, std::chrono::steady_clock::time_point now) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Steering (on/off, as in RC_steer).
    if (steer > 0) {
        gpio_.set(pins_.steer_in1, true);
        gpio_.set(pins_.steer_in2, false);
    } else if (steer < 0) {
        gpio_.set(pins_.steer_in1, false);
        gpio_.set(pins_.steer_in2, true);
    } else {
        gpio_.set(pins_.steer_in1, false);
        gpio_.set(pins_.steer_in2, false);
    }

    Dir dir;
    int magnitude = 0;
    if (brake) {
        dir = Dir::Brake;
    } else if (drive_permille > 0) {
        dir = Dir::Forward;
        magnitude = std::min(drive_permille, 1000);
    } else if (drive_permille < 0) {
        dir = Dir::Reverse;
        magnitude = std::min(-drive_permille, 1000);
    } else {
        dir = Dir::Coast;
    }

    // Start kick: when motion starts from rest or reverses direction, apply a
    // strong pulse for kick_.duration_ms to overcome static friction (RC_drive).
    const bool moving = dir == Dir::Forward || dir == Dir::Reverse;
    if (moving && magnitude > 0 && dir != last_dir_ && kick_.duration_ms > 0 && kick_.permille > 0) {
        kicking_ = true;
        kick_until_ = now + std::chrono::milliseconds(kick_.duration_ms);
    }
    if (kicking_) {
        if (!moving || now >= kick_until_) kicking_ = false;
        else magnitude = std::max(magnitude, kick_.permille);
    }

    set_drive_locked(dir, magnitude);
    last_dir_ = dir;
}

void Drv8835::stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    gpio_.set(pins_.steer_in1, false);
    gpio_.set(pins_.steer_in2, false);
    set_drive_locked(Dir::Brake, 0);
    last_dir_ = Dir::Brake;
    kicking_ = false;
}

} // namespace rumicar::hw
