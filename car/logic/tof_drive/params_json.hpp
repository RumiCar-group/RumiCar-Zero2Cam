// RumiCar-Zero2Cam — strict params.json loader for the ToF driving logic.
#pragma once

#include <string>

#include "tof_logic.hpp"

namespace rumicar::tof {

// Parse the JSON text of params.json.  Every parameter must be present and no
// unknown key is allowed, so that a typo in a pack is rejected instead of being
// silently replaced by a default.  Throws std::runtime_error with a message
// naming the offending key.
Params parse_params_json(const std::string& json_text);

// Serialise parameters in the same layout (used by tooling and tests).
std::string params_to_json(const Params& p);

} // namespace rumicar::tof
