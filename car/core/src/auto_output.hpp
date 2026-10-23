// What goes to the drive motor in auto mode (docs 3.5).
//
// The logic runs in every mode (its decisions are shown on the PC), so when
// the car is switched to auto — from manual, from record_only, or when an
// emergency stop is cleared — the logic may already be asking for full speed:
// with nothing in front it has been "driving" all along.  Without care the
// car would leap forward.  This limiter starts every auto run from rest: the
// drive allowed in auto grows from 0 at accel_permille_per_s (a start ramp)
// and is always capped at the auto limit the PC sets (set_auto_limit).  Once
// the ramp has reached the cap it has no further effect, so the logic's own
// speed control (slowing down, braking, reversing) is untouched while it
// drives.  Pure computation; the loop passes the time step.
#pragma once

#include <algorithm>

namespace rumicar {

class AutoOutputLimiter {
public:
    // accel_permille_per_s: growth of the allowed drive after a start; 0 = no start ramp.
    explicit AutoOutputLimiter(int accel_permille_per_s) : accel_(accel_permille_per_s) {}

    // A frame in which auto is not driving the motors (another mode, an
    // emergency stop, a logic error): the next auto run starts from rest.
    void idle() { ramp_ = 0.0; }

    // A frame driven by the logic in auto.  `drive` is the logic's command,
    // `limit` the auto limit (0..1000), `dt_s` the time since the last frame
    // (clamped to 0.2 s).  Returns the drive to output.
    int drive(int drive, int limit, double dt_s) {
        limit = std::clamp(limit, 0, 1000);
        if (accel_ <= 0) {
            ramp_ = 1000.0;
        } else {
            ramp_ = std::min(1000.0, ramp_ + accel_ * std::clamp(dt_s, 0.0, 0.2));
        }
        const int allowed = std::min(limit, static_cast<int>(ramp_));
        return std::clamp(drive, -allowed, allowed);
    }

    // True while the start ramp still limits (for logging and tests).
    bool ramping() const { return ramp_ < 1000.0; }

private:
    int accel_;
    double ramp_ = 0.0;
};

} // namespace rumicar
