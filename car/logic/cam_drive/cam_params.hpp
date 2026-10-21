// RumiCar-Zero2Cam — parameters of the camera driving logic (params.json of a
// cam_drive pack): the ToF driving parameters under "tof" plus the behaviour
// at traffic lights under "signal".  Strict: every key required, no unknown
// keys (docs 3.11).
#pragma once

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

struct CamParams {
    tof::Params tof;
    SignalParams signal;
};

// Returns an empty string when valid.
std::string validate(const SignalParams& p);

// Parse params.json of a cam_drive pack.  Throws std::runtime_error.
CamParams parse_cam_params_json(const std::string& json_text);

std::string cam_params_to_json(const CamParams& p);

} // namespace rumicar::cam
