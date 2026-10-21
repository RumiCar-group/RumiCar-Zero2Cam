// Unit tests for the ToF driving logic (pure computation, deterministic).
#include <cstdlib>
#include <string>
#include <vector>

#include "check.hpp"
#include "params_json.hpp"
#include "tof_logic.hpp"

using namespace rumicar::tof;

namespace {

constexpr int OPEN = RUMI_TOF_NO_TARGET;

rumi_sensor_frame frame(uint32_t seq, uint64_t t_ms, int L, int C, int R) {
    rumi_sensor_frame f{};
    f.size = sizeof(f);
    f.seq = seq;
    f.t_us = t_ms * 1000ull;
    f.tof_mm[RUMI_TOF_LEFT] = L;
    f.tof_mm[RUMI_TOF_CENTER] = C;
    f.tof_mm[RUMI_TOF_RIGHT] = R;
    return f;
}

struct Run {
    TofDriveLogic logic;
    uint32_t seq = 0;
    uint64_t t_ms = 1000;
    rumi_command last{};
    explicit Run(const Params& p) : logic(p) {}
    // Advance one frame of `dt_ms` with the given readings.
    const rumi_command& step(int L, int C, int R, uint64_t dt_ms = 50) {
        t_ms += dt_ms;
        rumi_sensor_frame f = frame(seq++, t_ms, L, C, R);
        bool ok = logic.step(f, last);
        CHECK(ok);
        return last;
    }
};

void test_ramp_and_accel_limit() {
    Params p;
    Run r(p);
    int prev = 0;
    bool reached = false;
    for (int i = 0; i < 60; ++i) {
        const auto& c = r.step(OPEN, OPEN, OPEN);
        CHECK(c.state == static_cast<uint8_t>(State::Cruise));
        CHECK(c.steer == RUMI_STEER_CENTER);
        CHECK(c.drive_permille >= prev);
        // accel limit: 1125/s * 0.05 s = 56.25 per frame (+1 for rounding)
        CHECK_MSG(c.drive_permille - prev <= 57, "frame " + std::to_string(i));
        prev = c.drive_permille;
        if (c.drive_permille == p.v_max) reached = true;
    }
    CHECK(reached);
    CHECK(prev == p.v_max);
}

void test_speed_cap_stops_and_resumes_smoothly() {
    // An external cap (used by cam_drive to stop at a red light) brings the
    // drive down at the deceleration rate, holds it at 0, and lifting the cap
    // ramps back up at the acceleration rate; steering and state are untouched.
    Params p;
    Run r(p);
    for (int i = 0; i < 60; ++i) r.step(OPEN, OPEN, OPEN);
    CHECK(r.last.drive_permille == p.v_max);
    r.logic.set_speed_cap(0);
    int prev = r.last.drive_permille;
    int frames_to_stop = 0;
    for (int i = 0; i < 40 && prev > 0; ++i) {
        const auto& c = r.step(OPEN, OPEN, OPEN);
        CHECK(c.drive_permille <= prev);
        CHECK_MSG(prev - c.drive_permille <= 151, "decel 3000/s * 0.05 s = 150 per frame");   // +1 rounding
        CHECK(c.state == static_cast<uint8_t>(State::Cruise) && c.steer == RUMI_STEER_CENTER);
        prev = c.drive_permille;
        ++frames_to_stop;
    }
    CHECK(prev == 0);
    CHECK(frames_to_stop >= 6 && frames_to_stop <= 8);   // 900 / 150 = 6 frames
    for (int i = 0; i < 10; ++i) CHECK(r.step(OPEN, OPEN, OPEN).drive_permille == 0);
    CHECK((r.last.flags & RUMI_CMD_FLAG_BRAKE) == 0);     // the cap itself does not brake
    // A partial cap limits, it does not stop.
    r.logic.set_speed_cap(300);
    for (int i = 0; i < 30; ++i) r.step(OPEN, OPEN, OPEN);
    CHECK(r.last.drive_permille == 300);
    r.logic.set_speed_cap(RUMI_DRIVE_MAX);
    prev = r.last.drive_permille;
    for (int i = 0; i < 40; ++i) {
        const auto& c = r.step(OPEN, OPEN, OPEN);
        CHECK(c.drive_permille >= prev);
        CHECK(c.drive_permille - prev <= 57);
        prev = c.drive_permille;
    }
    CHECK(prev == p.v_max);
    // The cap never affects reversing: a recovery still reverses at v_reverse.
    r.logic.set_speed_cap(0);
    for (int i = 0; i < 3; ++i) r.step(200, 50, 50);          // median accepts the wall -> emergency brake
    CHECK(r.last.state == static_cast<uint8_t>(State::EmergencyBrake));
    for (int i = 0; i < 4; ++i) r.step(200, 50, 50);          // brake_ms elapsed -> reverse
    CHECK(r.last.state == static_cast<uint8_t>(State::RecoverReverse));
    int min_drive = 0;
    for (int i = 0; i < 6; ++i) min_drive = std::min<int>(min_drive, r.step(200, 50, 50).drive_permille);
    CHECK(min_drive == -p.v_reverse);
    CHECK(r.last.aux_len == 0 && r.last.aux_format == 0);
}

void test_steer_modes_for_logics_on_top() {
    // HoldLane: no turning in front of an obstacle (speed still follows the
    // distance), but a near side wall is still avoided.  Force: the given
    // steer at once, hold time and all; last_turn_dir is untouched.  Neither
    // mode touches the emergency brake, and stuck detection is off meanwhile.
    Params p;
    Run r(p);
    for (int i = 0; i < 60; ++i) r.step(OPEN, OPEN, OPEN);
    r.logic.set_steer_mode(TofDriveLogic::SteerMode::HoldLane);
    for (int i = 0; i < 20; ++i) r.step(OPEN, 180, OPEN);   // an obstacle ahead, more room on the left
    CHECK(r.last.steer == RUMI_STEER_CENTER && r.last.state == static_cast<uint8_t>(State::Cruise));
    CHECK(r.last.drive_permille < p.v_max && r.last.drive_permille >= p.v_min);   // the speed curve still applies
    for (int i = 0; i < 10; ++i) r.step(40, 180, OPEN);     // a wall close on the left: move away from it
    CHECK(r.last.steer == RUMI_STEER_RIGHT);
    for (int i = 0; i < 10; ++i) r.step(OPEN, 180, OPEN);
    CHECK(r.last.steer == RUMI_STEER_CENTER);
    // No stuck recovery while held: the distance has not changed for far longer than stuck_time_ms.
    for (int i = 0; i < 60; ++i) r.step(OPEN, 180, OPEN);   // 3 s
    CHECK(r.last.state == static_cast<uint8_t>(State::Cruise) && r.last.drive_permille > 0);
    // Force: left at once, even though the steering just changed.
    r.logic.set_steer_mode(TofDriveLogic::SteerMode::Force, RUMI_STEER_LEFT);
    r.step(OPEN, 180, OPEN);
    CHECK(r.last.steer == RUMI_STEER_LEFT && r.last.state == static_cast<uint8_t>(State::TurnLeft));
    r.logic.set_steer_mode(TofDriveLogic::SteerMode::Force, RUMI_STEER_RIGHT);
    r.step(OPEN, 180, OPEN);
    CHECK(r.last.steer == RUMI_STEER_RIGHT && r.last.state == static_cast<uint8_t>(State::TurnRight));
    CHECK(r.logic.steer_mode() == TofDriveLogic::SteerMode::Force);
    // Back to Auto: the obstacle ahead now makes the logic turn by itself (towards the open side).
    r.logic.set_steer_mode(TofDriveLogic::SteerMode::Auto);
    for (int i = 0; i < 10; ++i) r.step(OPEN, 180, 100);
    CHECK(r.last.steer == RUMI_STEER_LEFT);
    // Emergency braking is never affected by a steering mode.
    r.logic.set_steer_mode(TofDriveLogic::SteerMode::Force, RUMI_STEER_RIGHT);
    for (int i = 0; i < 3; ++i) r.step(OPEN, 50, OPEN);
    CHECK(r.last.state == static_cast<uint8_t>(State::EmergencyBrake) && r.last.drive_permille == 0);
    CHECK((r.last.flags & RUMI_CMD_FLAG_BRAKE) != 0);
    // closing_mm_s and drive are exposed for the logic on top.
    Run q(p);
    for (int i = 0; i < 60; ++i) q.step(OPEN, OPEN, OPEN);
    CHECK(q.logic.drive() == p.v_max);
    q.step(OPEN, 240, OPEN);
    q.step(OPEN, 240, OPEN);
    q.step(OPEN, 240, OPEN);
    q.step(OPEN, 200, OPEN);   // median still 240
    q.step(OPEN, 200, OPEN);   // median 200 -> filtered 200 (a closer reading is taken at once): 40 mm in 50 ms
    CHECK(q.logic.closing_mm_s() > 700.0 && q.logic.closing_mm_s() < 900.0);
}

void test_speed_follows_distance() {
    Params p;
    Run r(p);
    for (int i = 0; i < 60; ++i) r.step(OPEN, OPEN, OPEN);
    // 1 s at 200 mm (shorter than stuck_time_ms): midway between d_min 150 and
    // d_max 250 -> midway between v_min and v_max.
    for (int i = 0; i < 20; ++i) r.step(OPEN, 200, OPEN);
    const int expected = (p.v_min + p.v_max) / 2;
    CHECK_MSG(std::abs(r.last.drive_permille - expected) <= 2, std::to_string(r.last.drive_permille));
    CHECK(r.last.state == static_cast<uint8_t>(State::Cruise));
    // 150 mm is below d_turn_enter_mm: the logic turns (towards the larger side) at v_min.
    for (int i = 0; i < 20; ++i) r.step(OPEN, 150, OPEN);
    CHECK(r.last.drive_permille == p.v_min);
    CHECK(r.last.state == static_cast<uint8_t>(State::TurnLeft) || r.last.state == static_cast<uint8_t>(State::TurnRight));
}

void test_stuck_requires_constant_distance() {
    Params p;
    Run r(p);
    for (int i = 0; i < 20; ++i) r.step(OPEN, OPEN, OPEN);
    // 3 s driving towards a wall whose distance changes by more than stuck_delta_mm
    // every few frames: never classified as stuck.
    for (int i = 0; i < 60; ++i) r.step(OPEN, 240 - (i % 4) * 20, OPEN);
    CHECK(r.last.state != static_cast<uint8_t>(State::RecoverReverse));
}

void test_emergency_brake_then_recover() {
    Params p;
    Run r(p);
    for (int i = 0; i < 60; ++i) r.step(OPEN, OPEN, OPEN);
    CHECK(r.last.drive_permille == p.v_max);

    // Obstacle appears very close: open on the left, wall on the right.
    // Three frames so the median filter (window 3) accepts the new value.
    r.step(200, 50, 50);
    r.step(200, 50, 50);
    const auto& c = r.step(200, 50, 50);
    CHECK(c.state == static_cast<uint8_t>(State::EmergencyBrake));
    CHECK(c.drive_permille == 0);
    CHECK((c.flags & RUMI_CMD_FLAG_BRAKE) != 0);

    // After brake_ms (150 ms = 3 frames) still blocked -> reverse, wheels turned RIGHT (away from open LEFT).
    r.step(200, 50, 50);
    r.step(200, 50, 50);
    r.step(200, 50, 50);
    const auto& rec = r.step(200, 50, 50);
    CHECK(rec.state == static_cast<uint8_t>(State::RecoverReverse));
    CHECK(rec.steer == RUMI_STEER_RIGHT);
    CHECK(rec.drive_permille < 0);
    CHECK((rec.flags & RUMI_CMD_FLAG_BRAKE) == 0);

    // Reverse reaches -v_reverse and ends within reverse_ms, then returns to cruise.
    int min_drive = 0;
    bool back_to_cruise = false;
    for (int i = 0; i < 20; ++i) {
        const auto& x = r.step(200, 50, 50);
        min_drive = std::min<int>(min_drive, x.drive_permille);
        if (x.state == static_cast<uint8_t>(State::Cruise) || x.state == static_cast<uint8_t>(State::TurnLeft)) {
            back_to_cruise = true;
            break;
        }
    }
    CHECK(min_drive == -p.v_reverse);
    CHECK(back_to_cruise);
}

void test_recover_exits_early_when_room_regained() {
    Params p;
    Run r(p);
    for (int i = 0; i < 40; ++i) r.step(OPEN, OPEN, OPEN);
    for (int i = 0; i < 8; ++i) r.step(200, 50, 50);
    CHECK(r.last.state == static_cast<uint8_t>(State::RecoverReverse));
    // Room regained: centre above d_turn_exit for the minimum reverse time (200 ms = 4 frames).
    for (int i = 0; i < 6; ++i) r.step(200, 240, 200);
    CHECK(r.last.state != static_cast<uint8_t>(State::RecoverReverse));
}

void test_steering_hysteresis_and_hold() {
    Params p;
    Run r(p);
    for (int i = 0; i < 10; ++i) r.step(OPEN, OPEN, OPEN);

    // Centre below d_turn_enter (200) with more room on the right -> turn right.
    for (int i = 0; i < 6; ++i) r.step(100, 190, 220);
    CHECK(r.last.steer == RUMI_STEER_RIGHT);
    CHECK(r.last.state == static_cast<uint8_t>(State::TurnRight));
    CHECK(r.last.drive_permille <= p.v_turn_cap);

    // Between enter (200) and exit (230): keep turning.
    for (int i = 0; i < 6; ++i) r.step(100, 215, 220);
    CHECK(r.last.steer == RUMI_STEER_RIGHT);

    // Above exit (230) and no side wall -> centre.
    for (int i = 0; i < 8; ++i) r.step(240, 245, 240);
    CHECK(r.last.steer == RUMI_STEER_CENTER);
    CHECK(r.last.state == static_cast<uint8_t>(State::Cruise));

    // Hold: a change requested immediately after a change is delayed by steer_hold_ms.
    uint64_t t_changed = 0;
    for (int i = 0; i < 12 && t_changed == 0; ++i) {
        r.step(100, 190, 220);
        if (r.last.steer == RUMI_STEER_RIGHT) t_changed = r.t_ms;
    }
    CHECK_MSG(t_changed != 0, "steer did not switch to RIGHT within 12 frames");
    r.step(220, 190, 100); // now more room on the left
    CHECK(r.last.steer == RUMI_STEER_RIGHT); // still within hold (50 ms < 150 ms)
    while (r.t_ms - t_changed < static_cast<uint64_t>(p.steer_hold_ms)) r.step(220, 190, 100);
    r.step(220, 190, 100);
    CHECK(r.last.steer == RUMI_STEER_LEFT);
}

void test_side_wall_avoidance() {
    Params p;
    Run r(p);
    for (int i = 0; i < 10; ++i) r.step(OPEN, OPEN, OPEN);
    for (int i = 0; i < 6; ++i) r.step(200, OPEN, 40); // right wall closer than d_side (60)
    CHECK(r.last.steer == RUMI_STEER_LEFT);
    for (int i = 0; i < 6; ++i) r.step(40, OPEN, 200);
    CHECK(r.last.steer == RUMI_STEER_RIGHT);
    for (int i = 0; i < 6; ++i) r.step(200, OPEN, 200);
    CHECK(r.last.steer == RUMI_STEER_CENTER);
}

void test_median_rejects_single_spike() {
    Params p;
    Run r(p);
    for (int i = 0; i < 20; ++i) r.step(OPEN, 220, OPEN);
    CHECK(r.last.state == static_cast<uint8_t>(State::Cruise));
    const int before = r.last.drive_permille;
    r.step(OPEN, 30, OPEN); // one-frame spike
    CHECK(r.last.state != static_cast<uint8_t>(State::EmergencyBrake));
    CHECK(r.last.drive_permille >= before - 1);
    r.step(OPEN, 220, OPEN);
    CHECK(r.last.state != static_cast<uint8_t>(State::EmergencyBrake));
}

void test_sensor_fault_stops_and_resumes() {
    Params p;
    Run r(p);
    for (int i = 0; i < 40; ++i) r.step(OPEN, OPEN, OPEN);
    // Centre sensor returns no reading.  Within invalid_hold_ms the last value is kept.
    r.step(OPEN, RUMI_TOF_INVALID, OPEN);
    CHECK(r.last.state == static_cast<uint8_t>(State::Cruise));
    for (int i = 0; i < 7; ++i) r.step(OPEN, RUMI_TOF_INVALID, OPEN); // 350 ms > 300 ms hold
    CHECK(r.last.state == static_cast<uint8_t>(State::SensorFault));
    CHECK(r.last.drive_permille == 0);
    CHECK((r.last.flags & RUMI_CMD_FLAG_FAULT) != 0);
    // Readings return -> cruise, with the drive ramping up again (not jumping).
    const auto& back = r.step(OPEN, OPEN, OPEN);
    CHECK(back.state == static_cast<uint8_t>(State::Cruise));
    CHECK((back.flags & RUMI_CMD_FLAG_FAULT) == 0);
    CHECK(back.drive_permille <= 57);
}

void test_stuck_detection() {
    Params p;
    Run r(p);
    for (int i = 0; i < 20; ++i) r.step(OPEN, 180, OPEN); // drive settles well above stuck_min_drive
    CHECK(r.last.drive_permille >= p.stuck_min_drive);
    CHECK(r.last.state != static_cast<uint8_t>(State::RecoverReverse));
    bool recovered = false;
    for (int i = 0; i < 40; ++i) { // 2 s at a constant 180 mm
        const auto& c = r.step(OPEN, 180, OPEN);
        if (c.state == static_cast<uint8_t>(State::RecoverReverse)) { recovered = true; break; }
    }
    CHECK(recovered);
}

void test_ttc_slows_when_closing_fast() {
    Params p;
    // Static obstacle at 200 mm.
    Run a(p);
    for (int i = 0; i < 20; ++i) a.step(OPEN, OPEN, OPEN);
    for (int i = 0; i < 20; ++i) a.step(OPEN, 200, OPEN);
    CHECK(a.last.state == static_cast<uint8_t>(State::Cruise));
    const int static_drive = a.last.drive_permille;
    // Approaching: 250 -> 200 over 100 ms (500 mm/s, ttc 0.4 s < 0.5 s).
    Run b(p);
    for (int i = 0; i < 20; ++i) b.step(OPEN, OPEN, OPEN);
    for (int i = 0; i < 20; ++i) b.step(OPEN, 250, OPEN);
    b.step(OPEN, 225, OPEN);
    b.step(OPEN, 200, OPEN);
    b.step(OPEN, 175, OPEN);
    CHECK_MSG(b.last.drive_permille < static_drive,
              std::to_string(b.last.drive_permille) + " vs " + std::to_string(static_drive));
}

void test_determinism() {
    Params p;
    std::vector<std::array<int, 3>> seq;
    for (int i = 0; i < 200; ++i) {
        int c = 260 - (i * 7) % 230;
        seq.push_back({(i * 13) % 300, c, (i * 29) % 300});
    }
    auto run = [&]() {
        std::vector<int> out;
        Run r(p);
        for (auto& s : seq) { const auto& c = r.step(s[0], s[1], s[2]); out.push_back(c.drive_permille * 10 + c.steer + 2); out.push_back(c.state); }
        return out;
    };
    CHECK(run() == run());
}

void test_params_json_strict() {
    Params p;
    const std::string text = params_to_json(p);
    Params q = parse_params_json(text);
    CHECK(params_to_json(q) == text);

    bool threw = false;
    try { parse_params_json("{\"conf_max_mm\": 250}"); } catch (const std::exception&) { threw = true; }
    CHECK_MSG(threw, "missing keys must be rejected");

    threw = false;
    try { parse_params_json(text.substr(0, text.rfind('}')) + ",\"typo_key\": 1}"); } catch (const std::exception&) { threw = true; }
    CHECK_MSG(threw, "unknown keys must be rejected");

    Params bad = p;
    bad.d_min_mm = 50; // violates d_emergency < d_min
    CHECK(!validate(bad).empty());
    bad = p;
    bad.v_max = 1200;
    CHECK(!validate(bad).empty());
    CHECK(validate(p).empty());
}

void test_malformed_frame_rejected() {
    Params p;
    TofDriveLogic l(p);
    rumi_sensor_frame f = frame(0, 1000, OPEN, OPEN, OPEN);
    f.size = 8;
    rumi_command c{};
    CHECK(!l.step(f, c));
}

} // namespace

int main() {
    test_ramp_and_accel_limit();
    test_speed_cap_stops_and_resumes_smoothly();
    test_steer_modes_for_logics_on_top();
    test_speed_follows_distance();
    test_stuck_requires_constant_distance();
    test_emergency_brake_then_recover();
    test_recover_exits_early_when_room_regained();
    test_steering_hysteresis_and_hold();
    test_side_wall_avoidance();
    test_median_rejects_single_spike();
    test_sensor_fault_stops_and_resumes();
    test_stuck_detection();
    test_ttc_slows_when_closing_fast();
    test_determinism();
    test_params_json_strict();
    test_malformed_frame_rejected();
    return check::summary("test_tof_logic");
}
