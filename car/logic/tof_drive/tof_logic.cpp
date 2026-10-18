// RumiCar-Zero2Cam — improved time-of-flight driving logic (pure computation).
#include "tof_logic.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace rumicar::tof {

namespace {

constexpr double kMaxDtSeconds = 0.2;   // larger gaps are clamped (e.g. after a pause)
constexpr uint64_t kRecoverMinUs = 200000; // reverse at least this long once started

bool between(int v, int lo, int hi) { return v >= lo && v <= hi; }

constexpr uint64_t ms_to_us(int ms) { return static_cast<uint64_t>(ms) * static_cast<uint64_t>(1000); }

} // namespace

std::string validate(const Params& p) {
    if (p.median_window != 1 && p.median_window != 3 && p.median_window != 5)
        return "median_window must be 1, 3 or 5";
    if (!(p.ema_alpha > 0.0 && p.ema_alpha <= 1.0)) return "ema_alpha must be in (0, 1]";
    if (p.conf_max_mm <= 0 || p.conf_max_mm >= TofDriveLogic::OPEN_MM) return "conf_max_mm out of range";
    if (p.invalid_hold_ms < 0 || p.invalid_hold_ms > 5000) return "invalid_hold_ms out of range";

    if (!(p.d_emergency_mm > 0 && p.d_emergency_mm < p.d_min_mm && p.d_min_mm < p.d_max_mm))
        return "require 0 < d_emergency_mm < d_min_mm < d_max_mm";
    if (p.d_max_mm > p.conf_max_mm) return "d_max_mm must not exceed conf_max_mm";
    if (!between(p.v_min, 0, RUMI_DRIVE_MAX) || !between(p.v_max, 0, RUMI_DRIVE_MAX) || p.v_min > p.v_max)
        return "require 0 <= v_min <= v_max <= 1000";
    if (!between(p.v_turn_cap, 0, RUMI_DRIVE_MAX)) return "v_turn_cap out of range";
    if (!between(p.v_reverse, 0, RUMI_DRIVE_MAX)) return "v_reverse out of range";
    if (p.accel_permille_per_s <= 0 || p.decel_permille_per_s <= 0) return "accel/decel must be positive";
    if (p.decel_permille_per_s < p.accel_permille_per_s) return "decel must be at least accel";
    if (!(p.ttc_slow_s >= 0.0 && p.ttc_slow_s <= 5.0)) return "ttc_slow_s out of range";

    if (!(p.d_turn_enter_mm > p.d_emergency_mm && p.d_turn_enter_mm < p.d_turn_exit_mm))
        return "require d_emergency_mm < d_turn_enter_mm < d_turn_exit_mm";
    if (p.d_turn_exit_mm > p.conf_max_mm) return "d_turn_exit_mm must not exceed conf_max_mm";
    if (p.side_deadband_mm < 0) return "side_deadband_mm must be >= 0";
    if (p.d_side_mm < 0 || p.d_side_mm >= p.conf_max_mm) return "d_side_mm out of range";
    if (p.steer_hold_ms < 0 || p.steer_hold_ms > 2000) return "steer_hold_ms out of range";

    if (p.brake_ms < 0 || p.brake_ms > 2000) return "brake_ms out of range";
    if (p.reverse_ms < 200 || p.reverse_ms > 5000) return "reverse_ms must be 200..5000";
    if (p.stuck_time_ms < 500 || p.stuck_time_ms > 10000) return "stuck_time_ms must be 500..10000";
    if (p.stuck_delta_mm < 0) return "stuck_delta_mm must be >= 0";
    if (!between(p.stuck_min_drive, 0, RUMI_DRIVE_MAX)) return "stuck_min_drive out of range";
    return {};
}

const char* state_name(State s) {
    switch (s) {
    case State::Cruise:         return "CRUISE";
    case State::TurnLeft:       return "TURN_LEFT";
    case State::TurnRight:      return "TURN_RIGHT";
    case State::EmergencyBrake: return "EMERGENCY_BRAKE";
    case State::RecoverReverse: return "RECOVER_REVERSE";
    case State::SensorFault:    return "SENSOR_FAULT";
    }
    return "UNKNOWN";
}

TofDriveLogic::TofDriveLogic(const Params& p) : p_(p) {}

// ---------------------------------------------------------------------------
// Sensor conditioning: raw -> (open / distance) -> median -> EMA -> filt_
// ---------------------------------------------------------------------------
int TofDriveLogic::median(const Sensor& s, int window) {
    const size_t n = static_cast<size_t>(std::clamp(std::min(s.count, window), 1, 5));
    std::array<int, 5> tmp{};
    for (size_t i = 0; i < n; ++i) {
        // most recent n samples ending at head-1
        const size_t idx = static_cast<size_t>((s.head + 5 - 1 - static_cast<int>(i)) % 5);
        tmp[i] = s.ring[idx];
    }
    // insertion sort on at most 5 elements
    for (size_t i = 1; i < n; ++i) {
        const int v = tmp[i];
        size_t j = i;
        while (j > 0 && tmp[j - 1] > v) { tmp[j] = tmp[j - 1]; --j; }
        tmp[j] = v;
    }
    return tmp[n / 2];
}

void TofDriveLogic::condition(int idx, int32_t raw, uint64_t t_us) {
    Sensor& s = sens_[static_cast<size_t>(idx)];

    if (raw < 0) {
        // No reading.  Hold the last value for a limited time, then flag missing.
        if (!s.have_valid || (t_us - s.last_valid_us) > ms_to_us(p_.invalid_hold_ms)) {
            s.missing = true;
        }
        filt_[static_cast<size_t>(idx)] = s.filt;
        return;
    }

    const int sample = (raw >= p_.conf_max_mm) ? OPEN_MM : raw;
    s.ring[static_cast<size_t>(s.head)] = sample;
    s.head = (s.head + 1) % 5;
    if (s.count < 5) ++s.count;
    s.have_valid = true;
    s.last_valid_us = t_us;
    s.missing = false;

    const int med = median(s, p_.median_window);
    if (med == OPEN_MM || s.filt == OPEN_MM || med < s.filt) {
        // No smoothing across the open/near boundary, and none when the
        // distance shrinks: a closer reading is accepted at once (safety).
        s.filt = med;
    } else {
        s.filt = static_cast<int>(std::lround(p_.ema_alpha * med + (1.0 - p_.ema_alpha) * s.filt));
    }
    filt_[static_cast<size_t>(idx)] = s.filt;
}

// ---------------------------------------------------------------------------
// Steering
// ---------------------------------------------------------------------------
void TofDriveLogic::decide_steer(int L, int C, int R, uint64_t t_us) {
    // Hysteresis on the "turning" condition.
    if (C < p_.d_turn_enter_mm) turning_ = true;
    else if (C > p_.d_turn_exit_mm) turning_ = false;

    int8_t desired;
    if (turning_) {
        if (L > R + p_.side_deadband_mm)      desired = RUMI_STEER_LEFT;
        else if (R > L + p_.side_deadband_mm) desired = RUMI_STEER_RIGHT;
        else                                  desired = last_turn_dir_;
    } else {
        const bool left_near  = L < p_.d_side_mm;
        const bool right_near = R < p_.d_side_mm;
        if (right_near && !left_near)      desired = RUMI_STEER_LEFT;
        else if (left_near && !right_near) desired = RUMI_STEER_RIGHT;
        else if (left_near && right_near) {
            if (L > R + p_.side_deadband_mm)      desired = RUMI_STEER_LEFT;
            else if (R > L + p_.side_deadband_mm) desired = RUMI_STEER_RIGHT;
            else                                  desired = RUMI_STEER_CENTER;
        } else {
            desired = RUMI_STEER_CENTER;
        }
    }

    if (desired != steer_) {
        const uint64_t held = t_us - steer_since_us_;
        if (held >= ms_to_us(p_.steer_hold_ms)) {
            steer_ = desired;
            steer_since_us_ = t_us;
        }
    }
    if (steer_ != RUMI_STEER_CENTER) last_turn_dir_ = steer_;
}

// ---------------------------------------------------------------------------
// Speed
// ---------------------------------------------------------------------------
int TofDriveLogic::speed_target(int C, double closing_mm_s, bool turning) const {
    int target;
    if (C >= OPEN_MM) {
        target = p_.v_max;
    } else {
        const double t = std::clamp(static_cast<double>(C - p_.d_min_mm) /
                                        static_cast<double>(p_.d_max_mm - p_.d_min_mm),
                                    0.0, 1.0);
        target = static_cast<int>(std::lround(p_.v_min + t * (p_.v_max - p_.v_min)));
        if (closing_mm_s > 0.0 && p_.ttc_slow_s > 0.0) {
            const double ttc = static_cast<double>(C) / closing_mm_s;
            if (ttc < p_.ttc_slow_s) target = std::min(target, p_.v_min);
        }
    }
    if (turning) target = std::min(target, p_.v_turn_cap);
    return target;
}

void TofDriveLogic::rate_limit(int target, double dt) {
    // Only a growing forward command is limited by the (slow) acceleration
    // rate.  Slowing down, stopping and building up reverse for a recovery use
    // the (fast) deceleration rate.
    const double tgt = static_cast<double>(target);
    const bool growing_forward = tgt > 0.0 && drive_ >= 0.0 && tgt > drive_;
    const double rate = (growing_forward ? p_.accel_permille_per_s : p_.decel_permille_per_s) * dt;
    const double delta  = std::clamp(tgt - drive_, -rate, rate);
    drive_ += delta;
}

void TofDriveLogic::enter(State s, uint64_t t_us) {
    state_ = s;
    state_since_us_ = t_us;
}

// ---------------------------------------------------------------------------
// Main step
// ---------------------------------------------------------------------------
bool TofDriveLogic::step(const rumi_sensor_frame& frame, rumi_command& out) {
    if (frame.size < sizeof(rumi_sensor_frame)) return false;

    const uint64_t t = frame.t_us;
    double dt = 0.0;
    if (have_time_ && t > last_t_us_) {
        dt = std::min(static_cast<double>(t - last_t_us_) / 1e6, kMaxDtSeconds);
    }
    have_time_ = true;
    last_t_us_ = t;
    if (state_since_us_ == 0 && state_ == State::Cruise) state_since_us_ = t;
    if (steer_since_us_ == 0) steer_since_us_ = t;

    for (int i = 0; i < RUMI_TOF_COUNT; ++i) condition(i, frame.tof_mm[i], t);

    const int L = filt_[RUMI_TOF_LEFT];
    const int C = filt_[RUMI_TOF_CENTER];
    const int R = filt_[RUMI_TOF_RIGHT];

    const bool any_missing = sensor_at(RUMI_TOF_LEFT).missing || sensor_at(RUMI_TOF_CENTER).missing ||
                             sensor_at(RUMI_TOF_RIGHT).missing;

    // Closing speed of the centre distance (positive = approaching).
    double closing = 0.0;
    if (dt > 0.0 && C < OPEN_MM && prev_c_filt_ < OPEN_MM) {
        closing = static_cast<double>(prev_c_filt_ - C) / dt;
    }
    prev_c_filt_ = C;

    uint32_t flags = 0;
    int target = 0;
    bool immediate_stop = false;

    // --- state transitions ---------------------------------------------------
    if (any_missing) {
        if (state_ != State::SensorFault) enter(State::SensorFault, t);
    } else if (state_ == State::SensorFault) {
        enter(State::Cruise, t);
        stuck_tracking_ = false;
    }

    switch (state_) {
    case State::SensorFault:
        immediate_stop = true;
        flags |= RUMI_CMD_FLAG_FAULT | RUMI_CMD_FLAG_BRAKE;
        break;

    case State::Cruise:
    case State::TurnLeft:
    case State::TurnRight: {
        if (C < p_.d_emergency_mm) {
            enter(State::EmergencyBrake, t);
            immediate_stop = true;
            flags |= RUMI_CMD_FLAG_BRAKE;
            stuck_tracking_ = false;
            break;
        }

        decide_steer(L, C, R, t);
        target = speed_target(C, closing, turning_);

        // Stuck detection: commanding forward drive but the centre distance does not change.
        const bool trackable = (drive_ >= p_.stuck_min_drive) && (C < OPEN_MM);
        if (!trackable) {
            stuck_tracking_ = false;
        } else if (!stuck_tracking_ || std::abs(C - stuck_ref_c_) > p_.stuck_delta_mm) {
            stuck_tracking_ = true;
            stuck_ref_c_ = C;
            stuck_ref_us_ = t;
        } else if ((t - stuck_ref_us_) >= ms_to_us(p_.stuck_time_ms)) {
            stuck_tracking_ = false;
            recover_steer_ = (L > R + p_.side_deadband_mm) ? RUMI_STEER_RIGHT
                           : (R > L + p_.side_deadband_mm) ? RUMI_STEER_LEFT
                           : (last_turn_dir_ == RUMI_STEER_LEFT ? RUMI_STEER_RIGHT : RUMI_STEER_LEFT);
            enter(State::RecoverReverse, t);
            steer_ = recover_steer_;
            steer_since_us_ = t;
            target = -p_.v_reverse;
            break;
        }

        State next = steer_ == RUMI_STEER_LEFT  ? State::TurnLeft
                   : steer_ == RUMI_STEER_RIGHT ? State::TurnRight
                                                : State::Cruise;
        if (next != state_) enter(next, t);
        break;
    }

    case State::EmergencyBrake: {
        immediate_stop = true;
        flags |= RUMI_CMD_FLAG_BRAKE;
        if ((t - state_since_us_) >= ms_to_us(p_.brake_ms)) {
            if (C >= p_.d_min_mm) {
                enter(State::Cruise, t);
                turning_ = false;
            } else {
                // Reverse with the wheels turned away from the open side so the nose swings towards it.
                recover_steer_ = (L > R + p_.side_deadband_mm) ? RUMI_STEER_RIGHT
                               : (R > L + p_.side_deadband_mm) ? RUMI_STEER_LEFT
                               : (last_turn_dir_ == RUMI_STEER_LEFT ? RUMI_STEER_RIGHT : RUMI_STEER_LEFT);
                enter(State::RecoverReverse, t);
                steer_ = recover_steer_;
                steer_since_us_ = t;
                immediate_stop = false;
                target = -p_.v_reverse;
                flags = 0;
            }
        }
        break;
    }

    case State::RecoverReverse: {
        const uint64_t elapsed = t - state_since_us_;
        const bool room = C >= p_.d_turn_exit_mm;
        if (elapsed >= ms_to_us(p_.reverse_ms) || (room && elapsed >= kRecoverMinUs)) {
            enter(State::Cruise, t);
            turning_ = false;
            stuck_tracking_ = false;
            // Steering is re-decided next frame; keep the recovery angle for this one.
            target = 0;
        } else {
            steer_ = recover_steer_;
            target = -p_.v_reverse;
        }
        break;
    }
    }

    // --- output --------------------------------------------------------------
    if (immediate_stop) {
        drive_ = 0.0;
    } else {
        rate_limit(target, dt);
    }

    out.size = sizeof(rumi_command);
    out.steer = steer_;
    out.drive_permille = static_cast<int16_t>(std::clamp(static_cast<int>(std::lround(drive_)),
                                                         RUMI_DRIVE_MIN, RUMI_DRIVE_MAX));
    out.state = static_cast<uint8_t>(state_);
    out.flags = flags;
    return true;
}

} // namespace rumicar::tof
