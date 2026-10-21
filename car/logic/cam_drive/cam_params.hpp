// RumiCar-Zero2Cam — parameters of the camera driving logic (params.json of a
// cam_drive pack): the ToF driving parameters under "tof", the behaviour at
// traffic lights under "signal" (always present), and the optional behaviours
// behind another vehicle ("vehicle") and at intersections ("intersection").
// An optional section that is absent means the behaviour is off; when it is
// present every key of it is required.  Strict: no unknown keys (docs 3.11).
#pragma once

#include <optional>
#include <string>

#include "tof_logic.hpp"

namespace rumicar::cam {

struct SignalParams {
    std::string red_detector = "traffic_light_red";    // name of the Local detector for a red light ("" = feature off)
    std::string green_detector = "traffic_light_green"; // name for a green light ("" = none: resume by time only)
    int stop_min_cells = 2;            // red is "close" when at least this many cells show it -> stop
    int approach_v = 500;              // speed cap (per mille) while red is seen but not yet close
    int min_stop_ms = 1000;            // stay stopped at least this long
    int resume_without_green_ms = 3000; // red not seen for this long while stopped -> resume (0 = only on green)
    int resume_ignore_ms = 1500;       // after resuming, ignore red for this long (passing the light)
};

// Following a vehicle ahead and passing a parked one (docs 3.10).
struct VehicleParams {
    std::string detector = "vehicle";  // name of the Local detector for a vehicle (required)
    int lane_cols = 4;                 // the vehicle is "in our lane" when its best cell lies in the centre this many columns
    int follow_gap_mm = 150;           // keep at least this centre distance behind it (speed cap 0 at/below)
    int follow_band_mm = 80;           // the cap rises from 0 at the gap to follow_v at gap + band
    int follow_v = 700;                // speed cap (per mille) while a vehicle is ahead
    int parked_confirm_ms = 2000;      // stopped behind a vehicle whose distance does not grow for this long -> it is parked
    int still_delta_mm = 20;           // the distance growing by more than this restarts that wait (it is moving off)
    std::string overtake_side = "open"; // pass on the "left", the "right", or the "open" side (more room; equal -> right)
    int side_clear_mm = 120;           // that side's ToF must read at least this (or nothing) before passing
    int overtake_v = 500;              // speed cap during the manoeuvre
    int out_ms = 500;                  // steer out (full lock towards the passing side) this long
    int pass_min_ms = 800;             // then drive straight at least this long ...
    int pass_max_ms = 3000;            // ... and at most this long ...
    int pass_clear_mm = 150;           // ... until the inner side ToF, having seen the vehicle (below this), reads at least this ...
    int pass_clear_ms = 300;           // ... for this long: the vehicle is passed
    int in_ms = 500;                   // steer back in this long
    int ignore_ms = 1500;              // afterwards ignore the vehicle detector this long
};

// Stopping at an intersection and going straight or turning (docs 3.10).
struct IntersectionParams {
    std::string left_detector = "intersection_left";   // detector for an intersection with a road to the left ("" = none)
    std::string right_detector = "intersection_right"; // ... to the right ("" = none); at least one is required
    int approach_v = 500;              // speed cap while an intersection is seen but the car is not yet at it
    std::string stop_trigger = "side_open"; // stop when a side ToF opens up ("side_open") or as soon as it is seen ("detector")
    int open_mm = 200;                 // a side ToF reading at/above this (or nothing) is an opening
    int stop_ms = 1000;                // stop this long
    std::string turn = "straight";     // then go "straight", turn "left", "right", or towards the "detector" that saw it
    int turn_v = 500;                  // speed cap while crossing and turning
    int cross_max_ms = 2000;           // drive straight at most this long waiting for the turn side to open
    int turn_ms = 900;                 // full-lock steering this long
    int ignore_ms = 3000;              // afterwards ignore the intersection detectors this long
};

struct CamParams {
    tof::Params tof;
    SignalParams signal;
    std::optional<VehicleParams> vehicle;
    std::optional<IntersectionParams> intersection;
};

// Each returns an empty string when valid.
std::string validate(const SignalParams& p);
std::string validate(const VehicleParams& p, const tof::Params& tof);
std::string validate(const IntersectionParams& p);

// Parse params.json of a cam_drive pack.  Throws std::runtime_error.
CamParams parse_cam_params_json(const std::string& json_text);

std::string cam_params_to_json(const CamParams& p);

} // namespace rumicar::cam
