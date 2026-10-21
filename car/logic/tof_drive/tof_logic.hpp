// RumiCar-Zero2Cam — improved time-of-flight driving logic (pure computation).
//
// This class contains no I/O.  It receives one sensor frame at a time together
// with the frame timestamp and produces a steering/drive command.  All timing
// is derived from the frame timestamps, so the same input sequence always
// yields the same output sequence (deterministic, replayable).
#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "rumicar/logic_api.h"

namespace rumicar::tof {

// Tunable parameters.  Loaded from params.json in the active pack.
struct Params {
    // --- sensor conditioning -------------------------------------------------
    int    conf_max_mm      = 250;  // readings at/above this are treated as "open" (nothing near)
    int    median_window    = 3;    // 1, 3 or 5 samples
    double ema_alpha        = 0.5;  // exponential smoothing weight of the newest sample (1.0 = none)
    int    invalid_hold_ms  = 300;  // keep the last value this long when a sensor returns no reading

    // --- speed ---------------------------------------------------------------
    int    d_emergency_mm   = 100;  // below: immediate brake
    int    d_min_mm         = 150;  // at/below: v_min
    int    d_max_mm         = 250;  // at/above: v_max
    int    v_min            = 500;  // per mille
    int    v_max            = 900;
    int    v_turn_cap       = 700;  // upper limit while steering
    int    v_reverse        = 700;  // magnitude used while recovering
    int    accel_permille_per_s = 1125; // 0 -> 900 in 0.8 s
    int    decel_permille_per_s = 3000; // 900 -> 0 in 0.3 s
    double ttc_slow_s       = 0.5;  // time-to-collision below which speed is held at v_min

    // --- steering ------------------------------------------------------------
    int    d_turn_enter_mm  = 200;  // start turning when the centre distance drops below this
    int    d_turn_exit_mm   = 230;  // stop turning when the centre distance rises above this
    int    side_deadband_mm = 20;   // |L - R| below this is "equal"
    int    d_side_mm        = 60;   // steer away from a side wall closer than this
    int    steer_hold_ms    = 150;  // minimum time between steering changes

    // --- recovery ------------------------------------------------------------
    int    brake_ms         = 150;  // hold the brake this long after an emergency stop
    int    reverse_ms       = 600;  // maximum reversing time per recovery
    int    stuck_time_ms    = 1500; // driving forward with no change in centre distance for this long = stuck
    int    stuck_delta_mm   = 15;   // "no change" threshold
    int    stuck_min_drive  = 300;  // stuck detection only when commanding at least this much drive
};

// Validate parameter ranges and ordering.  Returns an empty string when valid.
std::string validate(const Params& p);

enum class State : uint8_t {
    Cruise         = 0,
    TurnLeft       = 1,
    TurnRight      = 2,
    EmergencyBrake = 3,
    RecoverReverse = 4,
    SensorFault    = 5,
};

const char* state_name(State s);

class TofDriveLogic {
public:
    explicit TofDriveLogic(const Params& p);

    // Process one frame.  `out` is fully overwritten.  Returns false only when
    // the frame is malformed (e.g. wrong struct size); the caller must then stop.
    bool step(const rumi_sensor_frame& frame, rumi_command& out);

    State state() const { return state_; }

    // External upper limit on the forward drive command (per mille), applied
    // to the speed target before rate limiting, so a cap of 0 brings the car
    // to a smooth stop and lifting it resumes with the normal acceleration.
    // Used by logics built on top of this one (cam_drive stops at a red
    // light this way).  Reverse (recovery) and braking are not affected.
    void set_speed_cap(int permille) { speed_cap_ = permille; }
    int speed_cap() const { return speed_cap_; }

    // Filtered distances after conditioning (for telemetry/tests).  OPEN_MM when open.
    static constexpr int OPEN_MM = 9999;
    const std::array<int, RUMI_TOF_COUNT>& filtered() const { return filt_; }

private:
    struct Sensor {
        std::array<int, 5> ring{};   // raw samples (>= 0 or OPEN_MM)
        int      count = 0;          // samples stored (<= window)
        int      head = 0;
        int      filt = OPEN_MM;     // conditioned value
        bool     have_valid = false; // at least one valid sample ever
        uint64_t last_valid_us = 0;  // timestamp of the last valid raw reading
        bool     missing = false;    // invalid for longer than invalid_hold_ms
    };

    void condition(int idx, int32_t raw, uint64_t t_us);
    static int median(const Sensor& s, int window);
    void decide_steer(int L, int C, int R, uint64_t t_us);
    int  speed_target(int C, double closing_mm_s, bool turning) const;
    void rate_limit(int target, double dt);
    void enter(State s, uint64_t t_us);

    Params p_;
    std::array<Sensor, RUMI_TOF_COUNT> sens_{};
    std::array<int, RUMI_TOF_COUNT> filt_{OPEN_MM, OPEN_MM, OPEN_MM};

    State    state_ = State::Cruise;
    uint64_t state_since_us_ = 0;
    bool     have_time_ = false;
    uint64_t last_t_us_ = 0;

    bool     turning_ = false;
    int8_t   steer_ = RUMI_STEER_CENTER;
    uint64_t steer_since_us_ = 0;
    int8_t   last_turn_dir_ = RUMI_STEER_LEFT;  // used to break ties
    int8_t   recover_steer_ = RUMI_STEER_CENTER;

    double   drive_ = 0.0;           // current drive command (per mille), rate limited
    int      speed_cap_ = RUMI_DRIVE_MAX;
    int      prev_c_filt_ = OPEN_MM; // for closing-speed estimate

    // stuck detection
    bool     stuck_tracking_ = false;
    int      stuck_ref_c_ = 0;
    uint64_t stuck_ref_us_ = 0;

    const Sensor& sensor_at(int idx) const { return sens_[static_cast<size_t>(idx)]; }
};

} // namespace rumicar::tof
