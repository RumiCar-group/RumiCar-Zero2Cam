// What goes to the motors in a frame of the control loop (docs 3.4): the
// driving mode decides between the logic's command, the PC's manual command
// and braking, and the caps and ramps of each mode are applied.  RC_steer /
// RC_drive values; braked unless a mode says otherwise.  Pure computation (the
// loop passes the inputs and the time step and writes the result to the
// DRV8835), so the whole decision is testable as the loop runs it.
#pragma once

#include <algorithm>
#include <cstdint>

#include "auto_output.hpp"
#include "car_state.hpp"
#include "manual_output.hpp"
#include "rumicar/logic_api.h"

namespace rumicar {

struct FrameOutput {
    uint8_t steer = RUMI_STEER_CENTER;
    uint8_t steer_pwm = 0;
    uint8_t drive_dir = RUMI_DRIVE_BRAKE;
    uint8_t drive_pwm = 0;
    int kick_cap = 0;   // the highest duty a start kick may use (the mode's cap; 0 while braked)
};

class FrameOutputStage {
public:
    // auto_accel_pwm_per_s: the auto start ramp (core.json).
    explicit FrameOutputStage(int auto_accel_pwm_per_s) : auto_(auto_accel_pwm_per_s) {}

    // in: the car state of the frame (an expired heartbeat already turned
    // into the e-stop); rc / cmd: the logic's step() result and command;
    // max_drive_pwm: core.json's cap of every mode; dt_s: time since the last frame.
    FrameOutput step(const CarState::FrameInputs& in, int rc, const rumi_command& cmd, int max_drive_pwm, double dt_s) {
        FrameOutput out;
        const bool auto_driving = !in.estop && in.mode == DriveMode::Auto && rc == 0;
        if (!auto_driving) auto_.idle();   // the next auto run starts from rest
        if (in.estop || in.mode != DriveMode::Manual) manual_.idle();
        if (in.estop) return out;          // braked
        if (in.mode == DriveMode::Auto) {
            if (rc != 0) return out;       // braked
            out.steer = cmd.steer;
            out.steer_pwm = cmd.steer_pwm;
            if (cmd.flags & RUMI_CMD_FLAG_FAULT) return out;   // steering as the logic says, drive braked
            out.drive_dir = cmd.drive_dir;
            // Start ramp and the PC's auto limit (within max_drive_pwm).
            out.kick_cap = std::min(in.auto_limit, max_drive_pwm);
            out.drive_pwm = static_cast<uint8_t>(auto_.drive(cmd.drive_pwm, out.kick_cap, dt_s));
        } else if (in.mode == DriveMode::Manual) {
            out.steer = in.manual_steer;
            // RC_steer with the strength the PC sent (manual_drive's steer_pwm).
            out.steer_pwm = in.manual_steer == RUMI_STEER_CENTER ? 0 : in.manual_steer_pwm;
            out.drive_dir = in.manual_drive_dir;
            // The manual cap (within max_drive_pwm), then the manual acceleration.
            out.kick_cap = std::min(in.manual_max_pwm, max_drive_pwm);
            out.drive_pwm = static_cast<uint8_t>(
                manual_.drive(out.drive_dir, std::min<int>(in.manual_drive_pwm, out.kick_cap), in.manual_accel_pwm_per_s, dt_s));
        } // RecordOnly: braked
        return out;
    }

private:
    AutoOutputLimiter auto_;
    ManualOutputLimiter manual_;
};

} // namespace rumicar
