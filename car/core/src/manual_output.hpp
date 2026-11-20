// What goes to the drive motor in manual mode (docs 3.4).
//
// The PC sends the duty it wants at once: pressing the pad's edge or ↑ asks
// for the manual cap in one step.  A motor starting from rest at a high duty
// draws close to its stall current, and on a car whose motor and gears stall
// hard that dip of the battery can reset the Raspberry Pi (the Wi-Fi drops
// and the car restarts).  This limiter lets a rising duty climb at
// accel_pwm_per_s (duty per second, 0..255 scale) from where it is; a falling
// duty, FREE and BRAKE follow at once (stopping is never delayed), and a
// change of direction starts again from 0.  The rate is a drive setting the
// PC sets and the car keeps (manual_accel_pwm_per_s, default 287 = 0 to 255
// in about 0.9 s, the auto start ramp's default); 0 = no limit.  Pure
// computation; the loop passes the time step.
#pragma once

#include <algorithm>
#include <cstdint>

#include "rumicar/logic_api.h"

namespace rumicar {

class ManualOutputLimiter {
public:
    // A frame in which manual mode is not driving the motors (another mode,
    // an emergency stop): the next manual run starts from rest.
    void idle() {
        level_ = 0.0;
        dir_ = RUMI_DRIVE_FREE;
    }

    // A manual frame.  `dir` is RC_drive's direction, `pwm` the duty wanted
    // (already capped), `accel_pwm_per_s` the rate (0 = none), `dt_s` the time
    // since the last frame (clamped to 0.2 s).  Returns the duty to output.
    int drive(uint8_t dir, int pwm, int accel_pwm_per_s, double dt_s) {
        pwm = std::clamp(pwm, 0, RUMI_PWM_MAX);
        const bool moving = dir == RUMI_DRIVE_FORWARD || dir == RUMI_DRIVE_REVERSE;
        if (!moving || dir != dir_) level_ = 0.0;   // stopped, or reversing: from rest
        dir_ = dir;
        if (!moving) return pwm;
        if (accel_pwm_per_s <= 0) {
            level_ = pwm;
        } else {
            const double up = level_ + accel_pwm_per_s * std::clamp(dt_s, 0.0, 0.2);
            level_ = std::min(static_cast<double>(pwm), up);   // down at once, up at the rate
        }
        return static_cast<int>(level_);
    }

private:
    double level_ = 0.0;
    uint8_t dir_ = RUMI_DRIVE_FREE;
};

} // namespace rumicar
