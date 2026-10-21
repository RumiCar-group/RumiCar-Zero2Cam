// Tests of the camera driving logic: parameters, the traffic-light behaviour on
// synthetic frames (approach, stop, hold, green, resume, time-out), following
// and passing a vehicle, stopping and turning at intersections, how the
// behaviours give way to each other, the ToF-only fallback and the aux output.
// Usage: test_cam_logic <cam-base pack dir>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

#include "cam_logic.hpp"
#include "cam_params.hpp"
#include "check.hpp"
#include "params_json.hpp"
#include "vision_eval.hpp"
#include "vision_features.hpp"
#include "vision_pack.hpp"

using namespace rumicar;
using namespace rumicar::cam;

namespace {

constexpr unsigned W = 320, H = 240;
constexpr int OPEN = RUMI_TOF_NO_TARGET;

bool throws(const std::function<void()>& f, std::string* msg = nullptr) {
    try { f(); } catch (const std::exception& e) { if (msg) *msg = e.what(); return true; }
    return false;
}

std::string read_text(const std::string& path) {
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// A frame: grey road, optionally a coloured disc (a light) of radius r at (cx, cy).
struct Scene {
    std::vector<uint8_t> rgb;
    Scene() : rgb(static_cast<size_t>(W) * H * 3, 0) {
        for (unsigned y = 0; y < H; ++y) {
            for (unsigned x = 0; x < W; ++x) {
                uint8_t* p = rgb.data() + (static_cast<size_t>(y) * W + x) * 3;
                p[0] = p[1] = p[2] = static_cast<uint8_t>(90 + (y * 40) / H);   // unsaturated, "dark"
            }
        }
    }
    void disc(int cx, int cy, int r, uint8_t R, uint8_t G, uint8_t B) {
        for (int y = cy - r; y <= cy + r; ++y) {
            for (int x = cx - r; x <= cx + r; ++x) {
                if (x < 0 || y < 0 || x >= static_cast<int>(W) || y >= static_cast<int>(H)) continue;
                if ((x - cx) * (x - cx) + (y - cy) * (y - cy) > r * r) continue;
                uint8_t* p = rgb.data() + (static_cast<size_t>(y) * W + static_cast<size_t>(x)) * 3;
                p[0] = R; p[1] = G; p[2] = B;
            }
        }
    }
};

// A pack whose detectors fire on red / green cells (weights on the 3×3 window's
// centre cell counts), standardised with plausible statistics.
vision::VisionPack make_pack() {
    vision::VisionPack p;
    p.image_w = W;
    p.image_h = H;
    for (unsigned k = 0; k < vision::kFeaturesPerCell; ++k) { p.mean[k] = 0.0f; p.std[k] = 16.0f; }
    p.mean[8] = 8 * 8 * 100.0f;  p.std[8] = 2000.0f;   // ΣV
    p.mean[9] = 0.0f;            p.std[9] = 2000.0f;   // ΣS
    p.mean[10] = 0.0f;           p.std[10] = 500.0f;   // Σ|dx|
    p.mean[11] = 0.0f;           p.std[11] = 500.0f;   // Σ|dy|
    auto make = [&](const char* name, unsigned cls) {
        vision::Detector d;
        d.kind = vision::DetectorKind::Local;
        d.name = name;
        d.weights.assign(vision::kLocalDims, 0.0f);
        d.weights[4 * vision::kFeaturesPerCell + cls] = 8.0f;   // centre cell: 8 red pixels -> z=0.5 -> logit +1
        d.bias = -3.0f;
        d.thr_on = 0.6f;
        d.thr_off = 0.4f;
        d.confirm_frames = 2;
        d.release_frames = 3;
        d.min_cells = 1;
        d.row_min = 0;
        d.row_max = 5;
        return d;
    };
    p.detectors.push_back(make("traffic_light_red", vision::Red));
    p.detectors.push_back(make("traffic_light_green", vision::Green));
    return p;
}

// make_pack() plus a Local "vehicle" detector (blue) and Global
// "intersection_left" / "intersection_right" detectors that fire on a yellow
// patch in the left-middle cell (0,2) / a magenta patch in the right-middle
// cell (7,2) of the 8×6 grid.
vision::VisionPack make_full_pack() {
    vision::VisionPack p = make_pack();
    vision::Detector veh = p.detectors[0];
    veh.name = "vehicle";
    veh.weights.assign(vision::kLocalDims, 0.0f);
    veh.weights[4 * vision::kFeaturesPerCell + vision::Blue] = 8.0f;
    p.detectors.push_back(veh);
    auto global = [&](const char* name, unsigned cx, unsigned cy, unsigned cls) {
        vision::Detector d;
        d.kind = vision::DetectorKind::Global;
        d.name = name;
        d.weights.assign(p.config.dims(), 0.0f);
        d.weights[(cy * p.config.grid_x + cx) * vision::kFeaturesPerCell + cls] = 8.0f;
        d.bias = -3.0f;
        d.thr_on = 0.6f;
        d.thr_off = 0.4f;
        d.confirm_frames = 2;
        d.release_frames = 3;
        return d;
    };
    p.detectors.push_back(global("intersection_left", 0, 2, vision::Yellow));
    p.detectors.push_back(global("intersection_right", 7, 2, vision::Magenta));
    return p;
}

// A frame with another RumiCar ahead in our lane (a blue patch low in the
// centre, cell (4,4)), optionally with an intersection opening to the left
// and/or right in view.
Scene scene_with(bool vehicle, bool left = false, bool right = false) {
    Scene s;
    if (vehicle) s.disc(180, 180, 15, 30, 30, 230);
    if (left) s.disc(20, 100, 12, 230, 230, 20);
    if (right) s.disc(300, 100, 12, 230, 20, 230);
    return s;
}

struct Run {
    CamLogic logic;
    uint32_t seq = 0;
    uint64_t t_us = 1'000'000;
    rumi_command last{};
    Run(const CamParams& p, std::optional<vision::VisionPack> pack) : logic(p, std::move(pack)) {}
    int step(const Scene* scene, int L = OPEN, int C = OPEN, int R = OPEN, uint64_t dt_ms = 50) {
        t_us += dt_ms * 1000;
        rumi_sensor_frame f{};
        f.size = sizeof(f);
        f.seq = seq++;
        f.t_us = t_us;
        f.tof_mm[0] = L; f.tof_mm[1] = C; f.tof_mm[2] = R;
        if (scene) {
            f.image = scene->rgb.data();
            f.image_width = W;
            f.image_height = H;
            f.image_stride = W * 3;
        }
        return logic.step(f, last);
    }
    // Runs n frames; returns the last result code.
    int run(const Scene* scene, int n) { int rc = 0; for (int i = 0; i < n; ++i) rc = step(scene); return rc; }
    int run(const Scene* scene, int n, int L, int C, int R) { int rc = 0; for (int i = 0; i < n; ++i) rc = step(scene, L, C, R); return rc; }
    // Steps until the reported state is `state` (true) or `max` frames passed (false).
    bool until(uint8_t state, int max, const Scene* scene, int L = OPEN, int C = OPEN, int R = OPEN) {
        for (int i = 0; i < max; ++i) {
            step(scene, L, C, R);
            if (last.state == state) return true;
        }
        return false;
    }
    bool in(State st) const { return last.state == static_cast<uint8_t>(st); }
    bool in(tof::State st) const { return last.state == static_cast<uint8_t>(st); }
};

CamParams with_vehicle(const CamParams& p0) {
    CamParams p = p0;
    p.vehicle = VehicleParams{};
    return p;
}

CamParams with_intersection(const CamParams& p0, const std::string& turn = "straight", const std::string& trigger = "side_open") {
    CamParams p = p0;
    p.intersection = IntersectionParams{};
    p.intersection->turn = turn;
    p.intersection->stop_trigger = trigger;
    return p;
}

void test_params(const std::string& pack_dir) {
    const std::string text = read_text(pack_dir + "/params.json");
    const CamParams p = parse_cam_params_json(text);
    CHECK(p.signal.red_detector == "traffic_light_red" && p.signal.stop_min_cells == 2 && p.tof.v_max == 900);
    CHECK(parse_cam_params_json(cam_params_to_json(p)).signal.resume_ignore_ms == p.signal.resume_ignore_ms);
    std::string msg;
    CHECK(throws([&] { parse_cam_params_json("{}"); }, &msg) && msg.find("tof") != std::string::npos);
    CHECK(throws([&] { parse_cam_params_json("{\"tof\":{},\"signal\":{}}"); }, &msg) && msg.find("missing key") != std::string::npos);
    auto with = [&](const std::string& key, const std::string& value) {
        CamParams q = p;
        std::string j = cam_params_to_json(q);
        const auto pos = j.find("\"" + key + "\"");
        CHECK(pos != std::string::npos);
        const auto colon = j.find(':', pos);
        const auto end = j.find_first_of(",\n", colon);
        return j.substr(0, colon + 1) + " " + value + j.substr(end);
    };
    CHECK(throws([&] { parse_cam_params_json(with("stop_min_cells", "0")); }));
    CHECK(throws([&] { parse_cam_params_json(with("approach_v", "1001")); }));
    CHECK(throws([&] { parse_cam_params_json(with("red_detector", "\"Red!\"")); }));
    // No green detector: a time-out is required so the car cannot wait forever.
    {
        CamParams q = p;
        q.signal.green_detector = "";
        q.signal.resume_without_green_ms = 0;
        CHECK(!validate(q.signal).empty());
        q.signal.resume_without_green_ms = 3000;
        CHECK(validate(q.signal).empty());
    }
    CHECK(throws([&] { parse_cam_params_json(text + "x"); }));
    CHECK(throws([&] { parse_cam_params_json("{\"tof\":" + tof::params_to_json(p.tof) + ",\"signal\":{},\"extra\":1}"); }));

    // The optional sections: absent = off; present = every key, strictly.
    CHECK(!p.vehicle && !p.intersection);
    CamParams full = with_intersection(with_vehicle(p), "left");
    const std::string full_text = cam_params_to_json(full);
    const CamParams back = parse_cam_params_json(full_text);
    CHECK(back.vehicle && back.vehicle->detector == "vehicle" && back.vehicle->follow_gap_mm == 150 && back.vehicle->overtake_side == "open");
    CHECK(back.intersection && back.intersection->turn == "left" && back.intersection->stop_trigger == "side_open" && back.intersection->open_mm == 200);
    CHECK(cam_params_to_json(back) == full_text);
    auto with_full = [&](const std::string& key, const std::string& value) {
        std::string j = full_text;
        const auto pos = j.find("\"" + key + "\"");
        CHECK(pos != std::string::npos);
        const auto colon = j.find(':', pos);
        const auto end = j.find_first_of(",\n", colon);
        return j.substr(0, colon + 1) + " " + value + j.substr(end);
    };
    CHECK(throws([&] { parse_cam_params_json(with_full("overtake_side", "\"up\"")); }, &msg) && msg.find("overtake_side") != std::string::npos);
    CHECK(throws([&] { parse_cam_params_json(with_full("follow_gap_mm", "100")); }, &msg) && msg.find("d_emergency_mm") != std::string::npos);
    CHECK(throws([&] { parse_cam_params_json(with_full("follow_gap_mm", "250")); }, &msg) && msg.find("conf_max_mm") != std::string::npos);
    CHECK(throws([&] { parse_cam_params_json(with_full("pass_min_ms", "5000")); }));
    CHECK(throws([&] { parse_cam_params_json(with_full("turn", "\"around\"")); }, &msg) && msg.find("intersection.turn") != std::string::npos);
    CHECK(throws([&] { parse_cam_params_json(with_full("stop_trigger", "\"now\"")); }));
    CHECK(throws([&] { parse_cam_params_json(with_full("left_detector", "\"intersection_right\"")); }, &msg) && msg.find("differ") != std::string::npos);
    {
        std::string j = full_text;
        const auto pos = j.find("\"turn_ms\"");
        const auto end = j.find('\n', pos);
        j.erase(pos, end - pos + 1);   // drop a key of the section
        CHECK(throws([&] { parse_cam_params_json(j); }, &msg) && msg.find("missing key 'turn_ms'") != std::string::npos);
    }
    CHECK(throws([&] { parse_cam_params_json(full_text.substr(0, full_text.rfind('}')) + ",\"vehicle\":1}"); }));
    {
        CamParams q = full;
        q.intersection->left_detector = "";
        q.intersection->right_detector = "";
        CHECK(!validate(*q.intersection).empty());
        q.intersection->right_detector = "intersection_right";
        CHECK(validate(*q.intersection).empty());
        q.vehicle->lane_cols = 0;
        CHECK(!validate(*q.vehicle, q.tof).empty());
    }
}

void test_missing_detector_is_refused(const CamParams& p) {
    vision::VisionPack pack = make_pack();
    pack.detectors[0].name = "other";
    std::string msg;
    CHECK(throws([&] { CamLogic l(p, pack); }, &msg) && msg.find("traffic_light_red") != std::string::npos);
    // The vehicle and intersection sections name detectors too.
    CHECK(throws([&] { CamLogic l(with_vehicle(p), make_pack()); }, &msg) && msg.find("vehicle.detector") != std::string::npos);
    CHECK(throws([&] { CamLogic l(with_intersection(p), make_pack()); }, &msg) && msg.find("intersection.left_detector") != std::string::npos);
    CHECK(!throws([&] { CamLogic l(with_intersection(with_vehicle(p)), make_full_pack()); }));
    {
        vision::VisionPack g = make_full_pack();
        g.detectors[2].kind = vision::DetectorKind::Global;   // a vehicle detector must say where the vehicle is
        CHECK(throws([&] { CamLogic l(with_vehicle(p), g); }, &msg) && msg.find("local") != std::string::npos);
        CamParams q = with_vehicle(p);
        q.vehicle->lane_cols = 9;
        CHECK(throws([&] { CamLogic l(q, make_full_pack()); }, &msg) && msg.find("lane_cols") != std::string::npos);
    }
    // Without a vision.bin the sections are inert.
    CHECK(!throws([&] { CamLogic l(with_intersection(with_vehicle(p)), std::nullopt); }));
    // A pack without the green detector is fine when the parameters do not ask for one.
    CamParams q = p;
    q.signal.green_detector = "";
    vision::VisionPack red_only = make_pack();
    red_only.detectors.resize(1);
    CHECK(!throws([&] { CamLogic l(q, red_only); }));
}

void test_tof_only_without_vision(const CamParams& p) {
    Run r(p, std::nullopt);
    CHECK(!r.logic.has_vision());
    Scene s;
    s.disc(160, 100, 30, 230, 20, 20);   // a red light nobody detects
    for (int i = 0; i < 60; ++i) CHECK(r.step(&s) == 0);
    CHECK(r.last.drive_permille == p.tof.v_max);
    CHECK(r.last.state == static_cast<uint8_t>(tof::State::Cruise));
    CHECK(r.last.aux_len == 0 && r.last.aux_format == 0);
    CHECK(r.logic.aux_name(0) == nullptr);
    CHECK(std::string(state_name(r.last.state)) == "CRUISE");
    CHECK(std::string(state_name(17)) == "SIGNAL_STOP");
}

void test_red_light_stop_and_green_resume(const CamParams& p) {
    Run r(p, make_pack());
    CHECK(r.logic.has_vision());
    CHECK(std::string(r.logic.aux_name(0)) == "traffic_light_red" && std::string(r.logic.aux_name(1)) == "traffic_light_green" &&
          r.logic.aux_name(2) == nullptr);
    Scene road;
    for (int i = 0; i < 60; ++i) CHECK(r.step(&road) == 0);
    CHECK(r.last.drive_permille == p.tof.v_max);
    CHECK(r.last.aux_format == kAuxFormat && r.last.aux_len == 2 + 2 * kAuxDetectorBytes);
    CHECK(r.last.aux[0] == 8 && r.last.aux[1] == 6);
    CHECK((r.last.aux[2 + 4] & 0x3) == 0);   // red: not present, not active

    // A small red light far away: one cell -> approach (speed capped), not a stop.
    Scene far;
    far.disc(180, 100, 8, 230, 20, 20);   // ~3 small px wide inside cell (4,2): one cell lights up
    int rc = r.run(&far, 2);              // confirm_frames = 2
    CHECK(rc == 0);
    CHECK((r.last.aux[2 + 4] & 0x2) != 0);   // red active
    CHECK(r.last.state == static_cast<uint8_t>(State::SignalApproach));
    for (int i = 0; i < 30; ++i) r.step(&far);
    CHECK(r.last.drive_permille == p.signal.approach_v);
    CHECK(r.last.state == static_cast<uint8_t>(State::SignalApproach));

    // The light grows (closer): several cells -> stop, decelerating smoothly to 0 and braking.
    Scene near;
    near.disc(180, 100, 40, 230, 20, 20);
    int prev = r.last.drive_permille;
    r.step(&near);
    CHECK(r.last.state == static_cast<uint8_t>(State::SignalStop));
    bool monotone = true;
    for (int i = 0; i < 20; ++i) {
        r.step(&near);
        if (r.last.drive_permille > prev) monotone = false;
        prev = r.last.drive_permille;
    }
    CHECK(monotone);
    CHECK(r.last.drive_permille == 0);
    CHECK((r.last.flags & RUMI_CMD_FLAG_BRAKE) != 0);
    CHECK(r.last.steer == RUMI_STEER_CENTER);
    CHECK(r.last.aux[2 + 3] >= 2);   // cells
    // Red disappears but no green and the time-out (3 s) has not passed: still stopped.
    for (int i = 0; i < 30; ++i) r.step(&road);   // 1.5 s
    CHECK(r.last.state == static_cast<uint8_t>(State::SignalStop) && r.last.drive_permille == 0);
    // Green: resume at once (min_stop_ms long passed), then back to normal after resume_ignore_ms.
    Scene green;
    green.disc(180, 100, 40, 20, 230, 40);
    r.run(&green, 2);
    CHECK(r.last.state == static_cast<uint8_t>(State::SignalResume));
    prev = r.last.drive_permille;
    for (int i = 0; i < 10; ++i) {
        r.step(&green);
        CHECK(r.last.drive_permille >= prev);
        CHECK(r.last.drive_permille - prev <= 57);   // normal acceleration
        prev = r.last.drive_permille;
    }
    CHECK(prev > 0);
    // While resuming, a red light is ignored (the one just passed).
    r.run(&near, 5);
    CHECK(r.last.state == static_cast<uint8_t>(State::SignalResume));
    for (int i = 0; i < 30; ++i) r.step(&road);   // resume_ignore_ms (1.5 s) elapses
    CHECK(r.last.state == static_cast<uint8_t>(tof::State::Cruise));
    for (int i = 0; i < 30; ++i) r.step(&road);
    CHECK(r.last.drive_permille == p.tof.v_max);
    // And a red light seen again stops the car again.
    r.run(&near, 3);
    CHECK(r.last.state == static_cast<uint8_t>(State::SignalStop));
}

void test_red_light_timeout_resume(const CamParams& p0) {
    CamParams p = p0;
    p.signal.green_detector = "";
    p.signal.min_stop_ms = 500;
    p.signal.resume_without_green_ms = 2000;
    Run r(p, make_pack());
    Scene road, near;
    near.disc(180, 100, 40, 230, 20, 20);
    r.run(&road, 60);
    r.run(&near, 25);
    CHECK(r.last.state == static_cast<uint8_t>(State::SignalStop) && r.last.drive_permille == 0);
    // Red stays visible: no resume however long.
    r.run(&near, 100);
    CHECK(r.last.state == static_cast<uint8_t>(State::SignalStop));
    // Red gone (release 3 frames), then 2 s without red -> resume.
    r.run(&road, 3 + 41);   // release (3 frames), then 2 s without red
    CHECK(r.last.state == static_cast<uint8_t>(State::SignalResume));
    CHECK(r.last.drive_permille > 0);
}

void test_frames_without_image_keep_the_decision(const CamParams& p) {
    Run r(p, make_pack());
    Scene road, near;
    near.disc(180, 100, 40, 230, 20, 20);
    r.run(&road, 60);
    r.run(&near, 3);
    CHECK(r.last.state == static_cast<uint8_t>(State::SignalStop));
    // The camera is slower than the loop: frames without an image keep the last detections.
    for (int i = 0; i < 20; ++i) CHECK(r.step(nullptr) == 0);
    CHECK(r.last.state == static_cast<uint8_t>(State::SignalStop) && r.last.drive_permille == 0);
    // An obstacle in front while stopped: the ToF logic's emergency behaviour is reported and acts.
    for (int i = 0; i < 4; ++i) r.step(&near, 200, 50, 50);
    CHECK(r.last.state == static_cast<uint8_t>(tof::State::EmergencyBrake));
    // A frame of the wrong size is an error (the core brakes): a pack made for another camera setting.
    rumi_sensor_frame f{};
    f.size = sizeof(f);
    f.t_us = r.t_us + 50000;
    f.tof_mm[0] = f.tof_mm[1] = f.tof_mm[2] = OPEN;
    std::vector<uint8_t> small(640 * 480 * 3, 0);
    f.image = small.data(); f.image_width = 640; f.image_height = 480; f.image_stride = 640 * 3;
    rumi_command c{};
    CHECK(r.logic.step(f, c) == 3);
    f.size = 4;
    CHECK(r.logic.step(f, c) == 2);
}

// --- vehicles ---------------------------------------------------------------------------------------

void test_follow_keeps_the_gap(const CamParams& p0) {
    const CamParams p = with_vehicle(p0);
    const VehicleParams& vp = *p.vehicle;
    Run r(p, make_full_pack());
    const Scene road, car = scene_with(true), car_aside = [] { Scene s; s.disc(30, 180, 15, 30, 30, 230); return s; }();
    r.run(&road, 60);
    CHECK(r.in(tof::State::Cruise) && r.last.drive_permille == p.tof.v_max);
    // A vehicle in our lane, seen by the camera only (ToF open): follow at follow_v, steering untouched.
    CHECK(r.until(static_cast<uint8_t>(State::Follow), 4, &car));
    r.run(&car, 20);
    CHECK(r.last.drive_permille == vp.follow_v);
    CHECK(r.logic.tof().steer_mode() == tof::TofDriveLogic::SteerMode::Auto);
    // A vehicle beside the lane is not followed.
    r.run(&car_aside, 10);
    CHECK(r.in(tof::State::Cruise));
    r.run(&car, 10);
    CHECK(r.in(State::Follow));
    // The ToF sees it: hold the lane (no turning around it), speed from the gap.
    r.run(&car, 10, OPEN, 240, OPEN);
    CHECK(r.in(State::Follow) && r.logic.tof().steer_mode() == tof::TofDriveLogic::SteerMode::HoldLane);
    CHECK(r.last.drive_permille == vp.follow_v && r.last.steer == RUMI_STEER_CENTER);   // 240 >= gap + band
    r.run(&car, 20, OPEN, 190, OPEN);
    CHECK(r.in(State::Follow) && r.last.drive_permille == 350);   // 700 * (190 - 150) / 80
    int prev = r.last.drive_permille;
    bool monotone = true;
    for (int i = 0; i < 20; ++i) {
        r.step(&car, OPEN, 150, OPEN);
        if (r.last.drive_permille > prev) monotone = false;
        prev = r.last.drive_permille;
    }
    CHECK(monotone && r.last.drive_permille == 0 && (r.last.flags & RUMI_CMD_FLAG_BRAKE) != 0);
    CHECK(r.in(State::Follow) && r.last.steer == RUMI_STEER_CENTER);
    // It moves off: the gap opens, we move again, at most follow_v.
    r.run(&car, 30, OPEN, 230, OPEN);
    CHECK(r.in(State::Follow) && r.last.drive_permille == vp.follow_v && (r.last.flags & RUMI_CMD_FLAG_BRAKE) == 0);
    // Gone (camera and ToF): the ToF logic's own driving again.
    r.run(&road, 10);
    CHECK(r.in(tof::State::Cruise));
    r.run(&road, 30);
    CHECK(r.last.drive_permille == p.tof.v_max);
    // Lost from the camera while something is still close ahead: no "parked"
    // decision without seeing a vehicle; after ignore_ms the ToF logic turns around it.
    r.run(&car, 10, OPEN, 150, OPEN);
    r.run(&car, 20, OPEN, 150, OPEN);
    CHECK(r.in(State::Follow) && r.last.drive_permille == 0);
    r.run(&road, 20, OPEN, 150, 100);   // 1 s: still following
    CHECK(r.in(State::Follow) && r.last.drive_permille == 0);
    r.run(&road, 12, OPEN, 150, 100);   // ignore_ms (1.5 s) without the vehicle in view
    CHECK(r.in(tof::State::TurnLeft) || r.in(tof::State::Cruise));
    r.run(&road, 10, OPEN, 150, 100);
    CHECK(r.in(tof::State::TurnLeft) && r.last.drive_permille > 0);   // the ToF logic turns towards the open side
}

void test_parked_vehicle_is_passed(const CamParams& p0) {
    const CamParams p = with_vehicle(p0);
    const VehicleParams& vp = *p.vehicle;
    Run r(p, make_full_pack());
    const Scene road, car = scene_with(true);
    r.run(&road, 60);
    r.run(&car, 10, OPEN, 240, OPEN);
    CHECK(r.until(static_cast<uint8_t>(State::Follow), 2, &car, OPEN, 240, OPEN));
    for (int i = 0; i < 40 && r.last.drive_permille > 0; ++i) r.step(&car, OPEN, 150, OPEN);
    CHECK(r.in(State::Follow) && r.last.drive_permille == 0);
    // It does not move for parked_confirm_ms (2 s): parked.  Both sides open -> pass on the right.
    int stopped_frames = 0;
    for (int i = 0; i < 100 && !r.in(State::OvertakeOut); ++i) {
        r.step(&car, OPEN, 150, OPEN);
        if (!r.in(State::OvertakeOut)) ++stopped_frames;
    }
    CHECK(r.in(State::OvertakeOut));
    CHECK_MSG(stopped_frames >= 38 && stopped_frames <= 42, std::to_string(stopped_frames) + " frames stopped before passing");
    CHECK(r.last.steer == RUMI_STEER_RIGHT);
    for (int i = 0; i < 9; ++i) {   // out_ms = 500 ms = 10 frames
        r.step(&car, OPEN, 150, OPEN);
        CHECK(r.in(State::OvertakeOut) && r.last.steer == RUMI_STEER_RIGHT && r.last.drive_permille <= vp.overtake_v);
    }
    CHECK(r.last.drive_permille > 0);
    // Alongside: lane held, the vehicle appears on the inner (left) side, then the side clears.
    CHECK(r.until(static_cast<uint8_t>(State::OvertakePass), 2, &road, OPEN, OPEN, OPEN));
    CHECK(r.logic.tof().steer_mode() == tof::TofDriveLogic::SteerMode::HoldLane);
    r.run(&road, 5, OPEN, OPEN, OPEN);
    CHECK(r.in(State::OvertakePass) && r.last.steer == RUMI_STEER_CENTER && r.last.drive_permille == vp.overtake_v);
    r.run(&road, 10, 60, OPEN, OPEN);    // the vehicle beside us on the left
    CHECK(r.in(State::OvertakePass));
    r.run(&road, 3, OPEN, OPEN, OPEN);   // clear, but not yet for pass_clear_ms
    CHECK(r.in(State::OvertakePass));
    CHECK(r.until(static_cast<uint8_t>(State::OvertakeIn), 6, &road, OPEN, OPEN, OPEN));
    CHECK(r.last.steer == RUMI_STEER_LEFT);
    for (int i = 0; i < 9; ++i) {
        r.step(&road);
        CHECK(r.in(State::OvertakeIn) && r.last.steer == RUMI_STEER_LEFT);
    }
    CHECK(r.until(static_cast<uint8_t>(tof::State::Cruise), 2, &road));
    // Afterwards the detector is ignored for ignore_ms (the vehicle just passed), then it counts again.
    r.run(&car, 20, OPEN, OPEN, OPEN);   // 1 s
    CHECK(r.in(tof::State::Cruise));
    CHECK(r.until(static_cast<uint8_t>(State::Follow), 14, &car));
}

void test_pass_ends_by_time_without_a_side_reading(const CamParams& p0) {
    const CamParams p = with_vehicle(p0);
    const VehicleParams& vp = *p.vehicle;
    Run r(p, make_full_pack());
    const Scene road, car = scene_with(true);
    r.run(&road, 60);
    r.run(&car, 30, OPEN, 150, OPEN);
    CHECK(r.until(static_cast<uint8_t>(State::OvertakeOut), 45, &car, OPEN, 150, OPEN));
    CHECK(r.until(static_cast<uint8_t>(State::OvertakePass), 12, &road));
    // The inner side never sees the vehicle: pass_max_ms (3 s) ends the pass.
    r.run(&road, 55);
    CHECK(r.in(State::OvertakePass));
    CHECK(r.until(static_cast<uint8_t>(State::OvertakeIn), 8, &road));
    CHECK(vp.pass_max_ms == 3000);
}

void test_waits_when_no_side_is_clear_and_prefers_the_chosen_side(const CamParams& p0) {
    CamParams p = with_vehicle(p0);
    Run r(p, make_full_pack());
    const Scene road, car = scene_with(true);
    r.run(&road, 60);
    r.run(&car, 30, 80, 150, 80);    // walls close on both sides
    CHECK(r.until(static_cast<uint8_t>(State::FollowWait), 45, &car, 80, 150, 80));
    CHECK(r.last.drive_permille == 0 && (r.last.flags & RUMI_CMD_FLAG_BRAKE) != 0 && r.last.steer == RUMI_STEER_CENTER);
    r.run(&car, 40, 80, 150, 80);
    CHECK(r.in(State::FollowWait));
    // It moves off: follow again.
    r.run(&car, 10, 80, 200, 80);
    CHECK(r.in(State::Follow) && r.last.drive_permille > 0);
    // Parked again, the left side opens: pass on the left (the open side).
    r.run(&car, 30, 80, 150, 80);
    CHECK(r.until(static_cast<uint8_t>(State::FollowWait), 45, &car, 80, 150, 80));
    r.run(&car, 5, 130, 150, 80);
    CHECK(r.in(State::OvertakeOut) && r.last.steer == RUMI_STEER_LEFT);
    // overtake_side = "left" with only the right side clear: wait.
    p.vehicle->overtake_side = "left";
    Run q(p, make_full_pack());
    q.run(&road, 60);
    q.run(&car, 30, 80, 150, OPEN);
    CHECK(q.until(static_cast<uint8_t>(State::FollowWait), 45, &car, 80, 150, OPEN));
    q.run(&car, 40, 80, 150, OPEN);
    CHECK(q.in(State::FollowWait));
    q.run(&car, 5, OPEN, 150, OPEN);
    CHECK(q.in(State::OvertakeOut) && q.last.steer == RUMI_STEER_LEFT);
}

void test_no_passing_at_a_red_light(const CamParams& p0) {
    const CamParams p = with_vehicle(p0);
    Run r(p, make_full_pack());
    const Scene road, car = scene_with(true);
    Scene car_red = scene_with(true);
    car_red.disc(180, 100, 40, 230, 20, 20);   // and a red light close by
    r.run(&road, 60);
    r.run(&car, 30, OPEN, 150, OPEN);
    CHECK(r.in(State::Follow) && r.last.drive_permille == 0);
    // The vehicle ahead is waiting at the light too: however long, no passing.
    r.run(&car_red, 100, OPEN, 150, OPEN);   // 5 s
    CHECK(r.in(State::SignalStop) && r.last.drive_permille == 0);
    // Green: the light's resume phase, then the parked wait starts afresh.
    Scene car_green = scene_with(true);
    car_green.disc(180, 100, 40, 20, 230, 40);
    r.run(&car_green, 5, OPEN, 150, OPEN);
    CHECK(r.in(State::Follow));   // resuming, but following is what the car does
    r.run(&car_green, 30, OPEN, 150, OPEN);   // resume_ignore_ms over: the parked wait counts from here
    CHECK(r.in(State::Follow));
    r.run(&car_green, 30, OPEN, 150, OPEN);
    CHECK(r.in(State::Follow));
    CHECK(r.until(static_cast<uint8_t>(State::OvertakeOut), 15, &car_green, OPEN, 150, OPEN));
    // A red light during the manoeuvre cuts it short: stop, and the ToF logic steers again afterwards.
    r.run(&car_red, 3, OPEN, OPEN, OPEN);
    CHECK(r.in(State::SignalStop));
    CHECK(r.logic.tof().steer_mode() == tof::TofDriveLogic::SteerMode::Auto);
}

// --- intersections ------------------------------------------------------------------------------------

void test_intersection_stop_and_straight(const CamParams& p0) {
    const CamParams p = with_intersection(p0);
    const IntersectionParams& ip = *p.intersection;
    Run r(p, make_full_pack());
    const Scene road, left = scene_with(false, true);
    r.run(&road, 60, 120, OPEN, 120);   // a corridor with walls on both sides
    CHECK(r.in(tof::State::Cruise) && r.last.drive_permille == p.tof.v_max);
    CHECK(r.until(static_cast<uint8_t>(State::IntersectionApproach), 4, &left, 120, OPEN, 120));
    r.run(&left, 20, 120, OPEN, 120);
    CHECK(r.in(State::IntersectionApproach) && r.last.drive_permille == ip.approach_v);
    // Lost from view before any side opened: nothing happened, and it can be seen again at once.
    r.run(&road, 5, 120, OPEN, 120);
    CHECK(r.in(tof::State::Cruise));
    CHECK(r.until(static_cast<uint8_t>(State::IntersectionApproach), 4, &left, 120, OPEN, 120));
    // The left wall ends: at the intersection -> stop.
    r.step(&left, OPEN, OPEN, 120);
    CHECK(r.until(static_cast<uint8_t>(State::IntersectionStop), 3, &left, OPEN, OPEN, 120));
    r.run(&left, 10, OPEN, OPEN, 120);
    CHECK(r.in(State::IntersectionStop) && r.last.drive_permille == 0 && (r.last.flags & RUMI_CMD_FLAG_BRAKE) != 0);
    // stop_ms later: straight on, and the detectors are ignored for ignore_ms.
    CHECK(r.until(static_cast<uint8_t>(tof::State::Cruise), 12, &left, OPEN, OPEN, 120));
    r.run(&left, 50, OPEN, OPEN, 120);   // 2.5 s
    CHECK(r.in(tof::State::Cruise) && r.last.drive_permille == p.tof.v_max);
    CHECK(r.until(static_cast<uint8_t>(State::IntersectionApproach), 15, &left, 120, OPEN, 120));
}

void test_intersection_turns(const CamParams& p0) {
    // turn = "left": the left side is already open when the car stops -> turn at once.
    {
        const CamParams p = with_intersection(p0, "left");
        Run r(p, make_full_pack());
        const Scene road, left = scene_with(false, true);
        r.run(&road, 60, 120, OPEN, 120);
        r.run(&left, 5, 120, OPEN, 120);
        CHECK(r.until(static_cast<uint8_t>(State::IntersectionStop), 3, &left, OPEN, OPEN, 120));
        CHECK(r.until(static_cast<uint8_t>(State::IntersectionTurn), 25, &left, OPEN, OPEN, 120));
        CHECK(r.last.steer == RUMI_STEER_LEFT);
        for (int i = 0; i < 16; ++i) {   // turn_ms = 900 ms = 18 frames
            r.step(&left, OPEN, OPEN, 120);
            CHECK(r.in(State::IntersectionTurn) && r.last.steer == RUMI_STEER_LEFT && r.last.drive_permille <= p.intersection->turn_v);
        }
        CHECK(r.last.drive_permille > 0);
        CHECK(r.until(static_cast<uint8_t>(tof::State::Cruise), 3, &road, 120, OPEN, 120) || r.in(tof::State::TurnLeft));
        CHECK(r.logic.tof().steer_mode() == tof::TofDriveLogic::SteerMode::Auto);
    }
    // turn = "right" when the left side opened first: cross straight until the right side opens.
    {
        const CamParams p = with_intersection(p0, "right");
        Run r(p, make_full_pack());
        const Scene road, left = scene_with(false, true);
        r.run(&road, 60, 120, OPEN, 120);
        r.run(&left, 5, 120, OPEN, 120);
        CHECK(r.until(static_cast<uint8_t>(State::IntersectionStop), 3, &left, OPEN, OPEN, 120));
        CHECK(r.until(static_cast<uint8_t>(State::IntersectionCross), 25, &left, OPEN, OPEN, 120));
        CHECK(r.last.steer == RUMI_STEER_CENTER && r.logic.tof().steer_mode() == tof::TofDriveLogic::SteerMode::HoldLane);
        r.run(&left, 10, OPEN, OPEN, 120);
        CHECK(r.in(State::IntersectionCross) && r.last.drive_permille == p.intersection->turn_v);
        CHECK(r.until(static_cast<uint8_t>(State::IntersectionTurn), 3, &left, OPEN, OPEN, OPEN));
        CHECK(r.last.steer == RUMI_STEER_RIGHT);
    }
    // ... and gives up after cross_max_ms when it never opens.
    {
        const CamParams p = with_intersection(p0, "right");
        Run r(p, make_full_pack());
        const Scene road, left = scene_with(false, true);
        r.run(&road, 60, 120, OPEN, 120);
        r.run(&left, 5, 120, OPEN, 120);
        CHECK(r.until(static_cast<uint8_t>(State::IntersectionCross), 30, &left, OPEN, OPEN, 120));
        r.run(&left, 38, OPEN, OPEN, 120);
        CHECK(r.in(State::IntersectionCross));
        CHECK(r.until(static_cast<uint8_t>(tof::State::Cruise), 4, &left, OPEN, OPEN, 120));
    }
    // turn = "detector": towards the side whose detector saw the intersection; both -> straight.
    {
        const CamParams p = with_intersection(p0, "detector");
        Run r(p, make_full_pack());
        const Scene road, right = scene_with(false, false, true), both = scene_with(false, true, true);
        r.run(&road, 60, 120, OPEN, 120);
        r.run(&right, 5, 120, OPEN, 120);
        CHECK(r.until(static_cast<uint8_t>(State::IntersectionStop), 3, &right, 120, OPEN, OPEN));
        CHECK(r.until(static_cast<uint8_t>(State::IntersectionTurn), 25, &right, 120, OPEN, OPEN));
        CHECK(r.last.steer == RUMI_STEER_RIGHT);
        r.run(&road, 100, 120, OPEN, 120);   // turn over, ignore over
        CHECK(r.in(tof::State::Cruise));
        r.run(&both, 5, 120, OPEN, 120);
        CHECK(r.until(static_cast<uint8_t>(State::IntersectionStop), 3, &both, OPEN, OPEN, OPEN));
        CHECK(r.until(static_cast<uint8_t>(tof::State::Cruise), 25, &both, OPEN, OPEN, OPEN));
    }
    // stop_trigger = "detector": stop as soon as the intersection is confirmed (no walls needed).
    {
        const CamParams p = with_intersection(p0, "straight", "detector");
        Run r(p, make_full_pack());
        const Scene road, left = scene_with(false, true);
        r.run(&road, 60);
        CHECK(r.until(static_cast<uint8_t>(State::IntersectionStop), 4, &left));
        CHECK(r.until(static_cast<uint8_t>(tof::State::Cruise), 30, &left));
    }
}

void test_intersection_waits_for_the_light(const CamParams& p0) {
    const CamParams p = with_intersection(p0, "left");
    Run r(p, make_full_pack());
    Scene left_red = scene_with(false, true);
    left_red.disc(180, 100, 40, 230, 20, 20);
    Scene left_green = scene_with(false, true);
    left_green.disc(180, 100, 40, 20, 230, 40);
    const Scene road, left = scene_with(false, true);
    r.run(&road, 60, 120, OPEN, 120);
    r.run(&left_red, 10, OPEN, OPEN, 120);
    CHECK(r.in(State::SignalStop) && r.last.drive_permille == 0);
    r.run(&left_red, 60, OPEN, OPEN, 120);   // 3 s at the red light: the intersection stop does not move off
    CHECK(r.in(State::SignalStop) && r.last.drive_permille == 0);
    r.run(&left_green, 2, OPEN, OPEN, 120);   // green confirmed: resuming
    CHECK(!r.in(State::SignalStop));
    // Resuming: the turn follows at once (the stop time long passed) and is what is reported.
    CHECK(r.until(static_cast<uint8_t>(State::IntersectionTurn), 3, &left, OPEN, OPEN, 120));
    CHECK(r.last.steer == RUMI_STEER_LEFT);
    int turn_frames = 1;
    while (r.in(State::IntersectionTurn) && turn_frames < 30) { r.step(&left, OPEN, OPEN, 120); ++turn_frames; }
    CHECK_MSG(turn_frames >= 17 && turn_frames <= 19, std::to_string(turn_frames) + " frames turning (turn_ms 900)");
    CHECK(r.in(State::SignalResume));   // turn done, red still ignored for the rest of resume_ignore_ms
}

void test_behaviours_together(const CamParams& p0) {
    // Following a vehicle into an intersection: the intersection stop wins the
    // speed, following keeps the lane; no passing while the intersection is handled.
    const CamParams p = with_intersection(with_vehicle(p0));
    Run r(p, make_full_pack());
    const Scene road, car_left = scene_with(true, true);
    r.run(&road, 60, 120, OPEN, 120);
    r.run(&car_left, 5, 120, OPEN, 120);
    CHECK(r.in(State::IntersectionApproach));
    r.run(&car_left, 5, OPEN, 150, 120);
    CHECK(r.in(State::IntersectionStop));
    r.run(&car_left, 25, OPEN, 150, 120);   // stop_ms over, straight: the vehicle ahead is still there
    CHECK(r.in(State::Follow) && r.last.drive_permille == 0);
    CHECK(r.last.aux_len == 2 + 5 * kAuxDetectorBytes);
    CHECK(std::string(r.logic.aux_name(3)) == "intersection_left" && (r.last.aux[2 + 3 * kAuxDetectorBytes + 4] & 0x2) != 0);
    CHECK(std::string(state_name(19)) == "FOLLOW" && std::string(state_name(27)) == "INTERSECTION_TURN");
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: test_cam_logic <cam-base pack dir>\n");
        return 2;
    }
    const std::string pack_dir = argv[1];
    test_params(pack_dir);
    const CamParams p = parse_cam_params_json(read_text(pack_dir + "/params.json"));
    test_missing_detector_is_refused(p);
    test_tof_only_without_vision(p);
    test_red_light_stop_and_green_resume(p);
    test_red_light_timeout_resume(p);
    test_frames_without_image_keep_the_decision(p);
    test_follow_keeps_the_gap(p);
    test_parked_vehicle_is_passed(p);
    test_pass_ends_by_time_without_a_side_reading(p);
    test_waits_when_no_side_is_clear_and_prefers_the_chosen_side(p);
    test_no_passing_at_a_red_light(p);
    test_intersection_stop_and_straight(p);
    test_intersection_turns(p);
    test_intersection_waits_for_the_light(p);
    test_behaviours_together(p);
    return check::summary("test_cam_logic");
}
