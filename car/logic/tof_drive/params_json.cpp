// RumiCar-Zero2Cam — strict params.json loader for the ToF driving logic.
#include "params_json.hpp"

#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>

namespace rumicar::tof {

namespace {

using json = nlohmann::json;

template <typename T>
void take(const json& j, std::set<std::string>& seen, const char* key, T& dst) {
    auto it = j.find(key);
    if (it == j.end()) throw std::runtime_error(std::string("params.json: missing key '") + key + "'");
    if constexpr (std::is_same_v<T, int>) {
        if (!it->is_number_integer()) throw std::runtime_error(std::string("params.json: '") + key + "' must be an integer");
    } else {
        if (!it->is_number()) throw std::runtime_error(std::string("params.json: '") + key + "' must be a number");
    }
    dst = it->get<T>();
    seen.insert(key);
}

} // namespace

Params parse_params_json(const std::string& json_text) {
    json j;
    try {
        j = json::parse(json_text);
    } catch (const json::exception& e) {
        throw std::runtime_error(std::string("params.json: parse error: ") + e.what());
    }
    if (!j.is_object()) throw std::runtime_error("params.json: top level must be an object");

    Params p;
    std::set<std::string> seen;

    take(j, seen, "conf_max_mm", p.conf_max_mm);
    take(j, seen, "median_window", p.median_window);
    take(j, seen, "ema_alpha", p.ema_alpha);
    take(j, seen, "invalid_hold_ms", p.invalid_hold_ms);

    take(j, seen, "d_emergency_mm", p.d_emergency_mm);
    take(j, seen, "d_min_mm", p.d_min_mm);
    take(j, seen, "d_max_mm", p.d_max_mm);
    take(j, seen, "v_min", p.v_min);
    take(j, seen, "v_max", p.v_max);
    take(j, seen, "v_turn_cap", p.v_turn_cap);
    take(j, seen, "v_reverse", p.v_reverse);
    take(j, seen, "accel_permille_per_s", p.accel_permille_per_s);
    take(j, seen, "decel_permille_per_s", p.decel_permille_per_s);
    take(j, seen, "ttc_slow_s", p.ttc_slow_s);

    take(j, seen, "d_turn_enter_mm", p.d_turn_enter_mm);
    take(j, seen, "d_turn_exit_mm", p.d_turn_exit_mm);
    take(j, seen, "side_deadband_mm", p.side_deadband_mm);
    take(j, seen, "d_side_mm", p.d_side_mm);
    take(j, seen, "steer_hold_ms", p.steer_hold_ms);

    take(j, seen, "brake_ms", p.brake_ms);
    take(j, seen, "reverse_ms", p.reverse_ms);
    take(j, seen, "stuck_time_ms", p.stuck_time_ms);
    take(j, seen, "stuck_delta_mm", p.stuck_delta_mm);
    take(j, seen, "stuck_min_drive", p.stuck_min_drive);

    for (auto it = j.begin(); it != j.end(); ++it) {
        if (!seen.count(it.key())) throw std::runtime_error("params.json: unknown key '" + it.key() + "'");
    }

    const std::string err = validate(p);
    if (!err.empty()) throw std::runtime_error("params.json: " + err);
    return p;
}

std::string params_to_json(const Params& p) {
    json j = json::object();
    j["conf_max_mm"] = p.conf_max_mm;
    j["median_window"] = p.median_window;
    j["ema_alpha"] = p.ema_alpha;
    j["invalid_hold_ms"] = p.invalid_hold_ms;
    j["d_emergency_mm"] = p.d_emergency_mm;
    j["d_min_mm"] = p.d_min_mm;
    j["d_max_mm"] = p.d_max_mm;
    j["v_min"] = p.v_min;
    j["v_max"] = p.v_max;
    j["v_turn_cap"] = p.v_turn_cap;
    j["v_reverse"] = p.v_reverse;
    j["accel_permille_per_s"] = p.accel_permille_per_s;
    j["decel_permille_per_s"] = p.decel_permille_per_s;
    j["ttc_slow_s"] = p.ttc_slow_s;
    j["d_turn_enter_mm"] = p.d_turn_enter_mm;
    j["d_turn_exit_mm"] = p.d_turn_exit_mm;
    j["side_deadband_mm"] = p.side_deadband_mm;
    j["d_side_mm"] = p.d_side_mm;
    j["steer_hold_ms"] = p.steer_hold_ms;
    j["brake_ms"] = p.brake_ms;
    j["reverse_ms"] = p.reverse_ms;
    j["stuck_time_ms"] = p.stuck_time_ms;
    j["stuck_delta_mm"] = p.stuck_delta_mm;
    j["stuck_min_drive"] = p.stuck_min_drive;
    return j.dump(2) + "\n";
}

} // namespace rumicar::tof
