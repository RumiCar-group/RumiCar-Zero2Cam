#include "cam_params.hpp"

#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>

#include "params_json.hpp"
#include "vision_pack.hpp"

namespace rumicar::cam {

using json = nlohmann::json;

namespace {

// Reads the keys of one section strictly: every key taken must exist and have
// the right type and range, and no other key may be present.
class Section {
public:
    Section(const json& j, const char* name) : j_(j), name_(name) {}

    int take_int(const char* key, int lo, int hi) {
        const auto it = find(key);
        if (!it->is_number_integer()) throw std::runtime_error(prefix(key) + " must be an integer");
        const auto v = it->get<long long>();
        if (v < lo || v > hi)
            throw std::runtime_error(prefix(key) + " must be in [" + std::to_string(lo) + ", " + std::to_string(hi) + "]");
        return static_cast<int>(v);
    }

    std::string take_string(const char* key) {
        const auto it = find(key);
        if (!it->is_string()) throw std::runtime_error(prefix(key) + " must be a string");
        return it->get<std::string>();
    }

    void no_unknown_keys() const {
        for (auto it = j_.begin(); it != j_.end(); ++it) {
            if (!seen_.count(it.key())) throw std::runtime_error("params.json: unknown key '" + name_ + "." + it.key() + "'");
        }
    }

private:
    const json& j_;
    std::string name_;
    std::set<std::string> seen_;

    std::string prefix(const char* key) const { return "params.json: " + name_ + "." + key; }

    json::const_iterator find(const char* key) {
        const auto it = j_.find(key);
        if (it == j_.end()) throw std::runtime_error("params.json: " + name_ + ": missing key '" + key + "'");
        seen_.insert(key);
        return it;
    }
};

bool valid_optional_name(const std::string& name) { return name.empty() || vision::valid_detector_name(name); }

SignalParams parse_signal(const json& j) {
    Section s(j, "signal");
    SignalParams p;
    p.red_detector = s.take_string("red_detector");
    p.green_detector = s.take_string("green_detector");
    p.stop_min_cells = s.take_int("stop_min_cells", 1, 4096);
    p.approach_v = s.take_int("approach_v", 0, RUMI_DRIVE_MAX);
    p.min_stop_ms = s.take_int("min_stop_ms", 0, 60000);
    p.resume_without_green_ms = s.take_int("resume_without_green_ms", 0, 600000);
    p.resume_ignore_ms = s.take_int("resume_ignore_ms", 0, 60000);
    s.no_unknown_keys();
    return p;
}

VehicleParams parse_vehicle(const json& j) {
    Section s(j, "vehicle");
    VehicleParams p;
    p.detector = s.take_string("detector");
    p.lane_cols = s.take_int("lane_cols", 1, 255);
    p.follow_gap_mm = s.take_int("follow_gap_mm", 1, 9000);
    p.follow_band_mm = s.take_int("follow_band_mm", 1, 9000);
    p.follow_v = s.take_int("follow_v", 0, RUMI_DRIVE_MAX);
    p.parked_confirm_ms = s.take_int("parked_confirm_ms", 100, 600000);
    p.still_delta_mm = s.take_int("still_delta_mm", 0, 9000);
    p.overtake_side = s.take_string("overtake_side");
    p.side_clear_mm = s.take_int("side_clear_mm", 0, 9000);
    p.overtake_v = s.take_int("overtake_v", 0, RUMI_DRIVE_MAX);
    p.out_ms = s.take_int("out_ms", 0, 60000);
    p.pass_min_ms = s.take_int("pass_min_ms", 0, 60000);
    p.pass_max_ms = s.take_int("pass_max_ms", 0, 60000);
    p.pass_clear_mm = s.take_int("pass_clear_mm", 0, 9000);
    p.pass_clear_ms = s.take_int("pass_clear_ms", 0, 60000);
    p.in_ms = s.take_int("in_ms", 0, 60000);
    p.ignore_ms = s.take_int("ignore_ms", 0, 60000);
    s.no_unknown_keys();
    return p;
}

IntersectionParams parse_intersection(const json& j) {
    Section s(j, "intersection");
    IntersectionParams p;
    p.left_detector = s.take_string("left_detector");
    p.right_detector = s.take_string("right_detector");
    p.approach_v = s.take_int("approach_v", 0, RUMI_DRIVE_MAX);
    p.stop_trigger = s.take_string("stop_trigger");
    p.open_mm = s.take_int("open_mm", 1, 9000);
    p.stop_ms = s.take_int("stop_ms", 0, 60000);
    p.turn = s.take_string("turn");
    p.turn_v = s.take_int("turn_v", 0, RUMI_DRIVE_MAX);
    p.cross_max_ms = s.take_int("cross_max_ms", 0, 60000);
    p.turn_ms = s.take_int("turn_ms", 0, 60000);
    p.ignore_ms = s.take_int("ignore_ms", 0, 60000);
    s.no_unknown_keys();
    return p;
}

} // namespace

std::string validate(const SignalParams& p) {
    if (!valid_optional_name(p.red_detector)) return "signal.red_detector is not a valid detector name";
    if (!valid_optional_name(p.green_detector)) return "signal.green_detector is not a valid detector name";
    if (p.red_detector.empty() && !p.green_detector.empty()) return "signal.green_detector needs signal.red_detector";
    if (p.stop_min_cells < 1 || p.stop_min_cells > 4096) return "signal.stop_min_cells must be 1..4096";
    if (p.approach_v < 0 || p.approach_v > RUMI_DRIVE_MAX) return "signal.approach_v out of range";
    if (p.min_stop_ms < 0 || p.min_stop_ms > 60000) return "signal.min_stop_ms out of range";
    if (p.resume_without_green_ms < 0 || p.resume_without_green_ms > 600000) return "signal.resume_without_green_ms out of range";
    if (p.green_detector.empty() && p.resume_without_green_ms == 0) return "signal.resume_without_green_ms must be > 0 without a green detector";
    if (p.resume_ignore_ms < 0 || p.resume_ignore_ms > 60000) return "signal.resume_ignore_ms out of range";
    return {};
}

std::string validate(const VehicleParams& p, const tof::Params& tof) {
    if (!vision::valid_detector_name(p.detector)) return "vehicle.detector is not a valid detector name";
    if (p.lane_cols < 1 || p.lane_cols > 255) return "vehicle.lane_cols must be 1..255";
    if (p.follow_gap_mm <= tof.d_emergency_mm) return "vehicle.follow_gap_mm must be greater than tof.d_emergency_mm";
    if (p.follow_gap_mm >= tof.conf_max_mm) return "vehicle.follow_gap_mm must be less than tof.conf_max_mm (the ToF cannot see the gap)";
    if (p.follow_band_mm < 1 || p.follow_band_mm > 9000) return "vehicle.follow_band_mm must be 1..9000";
    if (p.follow_v < 0 || p.follow_v > RUMI_DRIVE_MAX) return "vehicle.follow_v out of range";
    if (p.parked_confirm_ms < 100 || p.parked_confirm_ms > 600000) return "vehicle.parked_confirm_ms must be 100..600000";
    if (p.still_delta_mm < 0 || p.still_delta_mm > 9000) return "vehicle.still_delta_mm out of range";
    if (p.overtake_side != "left" && p.overtake_side != "right" && p.overtake_side != "open")
        return "vehicle.overtake_side must be \"left\", \"right\" or \"open\"";
    if (p.side_clear_mm < 0 || p.side_clear_mm > 9000) return "vehicle.side_clear_mm out of range";
    if (p.overtake_v < 0 || p.overtake_v > RUMI_DRIVE_MAX) return "vehicle.overtake_v out of range";
    if (p.out_ms < 0 || p.out_ms > 60000) return "vehicle.out_ms out of range";
    if (p.pass_min_ms < 0 || p.pass_max_ms > 60000 || p.pass_min_ms > p.pass_max_ms) return "require 0 <= vehicle.pass_min_ms <= vehicle.pass_max_ms <= 60000";
    if (p.pass_clear_mm < 0 || p.pass_clear_mm > 9000) return "vehicle.pass_clear_mm out of range";
    if (p.pass_clear_ms < 0 || p.pass_clear_ms > 60000) return "vehicle.pass_clear_ms out of range";
    if (p.in_ms < 0 || p.in_ms > 60000) return "vehicle.in_ms out of range";
    if (p.ignore_ms < 0 || p.ignore_ms > 60000) return "vehicle.ignore_ms out of range";
    return {};
}

std::string validate(const IntersectionParams& p) {
    if (!valid_optional_name(p.left_detector)) return "intersection.left_detector is not a valid detector name";
    if (!valid_optional_name(p.right_detector)) return "intersection.right_detector is not a valid detector name";
    if (p.left_detector.empty() && p.right_detector.empty()) return "intersection needs left_detector or right_detector";
    if (p.left_detector == p.right_detector) return "intersection.left_detector and right_detector must differ";
    if (p.approach_v < 0 || p.approach_v > RUMI_DRIVE_MAX) return "intersection.approach_v out of range";
    if (p.stop_trigger != "side_open" && p.stop_trigger != "detector") return "intersection.stop_trigger must be \"side_open\" or \"detector\"";
    if (p.open_mm < 1 || p.open_mm > 9000) return "intersection.open_mm out of range";
    if (p.stop_ms < 0 || p.stop_ms > 60000) return "intersection.stop_ms out of range";
    if (p.turn != "straight" && p.turn != "left" && p.turn != "right" && p.turn != "detector")
        return "intersection.turn must be \"straight\", \"left\", \"right\" or \"detector\"";
    if (p.turn_v < 0 || p.turn_v > RUMI_DRIVE_MAX) return "intersection.turn_v out of range";
    if (p.cross_max_ms < 0 || p.cross_max_ms > 60000) return "intersection.cross_max_ms out of range";
    if (p.turn_ms < 0 || p.turn_ms > 60000) return "intersection.turn_ms out of range";
    if (p.ignore_ms < 0 || p.ignore_ms > 60000) return "intersection.ignore_ms out of range";
    return {};
}

CamParams parse_cam_params_json(const std::string& json_text) {
    json j;
    try {
        j = json::parse(json_text);
    } catch (const json::exception& e) {
        throw std::runtime_error(std::string("params.json: parse error: ") + e.what());
    }
    if (!j.is_object()) throw std::runtime_error("params.json: top level must be an object");
    const auto tof_it = j.find("tof");
    if (tof_it == j.end() || !tof_it->is_object()) throw std::runtime_error("params.json: missing object 'tof'");
    const auto sig_it = j.find("signal");
    if (sig_it == j.end() || !sig_it->is_object()) throw std::runtime_error("params.json: missing object 'signal'");
    const auto veh_it = j.find("vehicle");
    if (veh_it != j.end() && !veh_it->is_object()) throw std::runtime_error("params.json: 'vehicle' must be an object");
    const auto int_it = j.find("intersection");
    if (int_it != j.end() && !int_it->is_object()) throw std::runtime_error("params.json: 'intersection' must be an object");
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (it.key() != "tof" && it.key() != "signal" && it.key() != "vehicle" && it.key() != "intersection")
            throw std::runtime_error("params.json: unknown key '" + it.key() + "'");
    }

    CamParams p;
    p.tof = tof::parse_params_json(tof_it->dump());
    p.signal = parse_signal(*sig_it);
    std::string err = validate(p.signal);
    if (!err.empty()) throw std::runtime_error("params.json: " + err);
    if (veh_it != j.end()) {
        p.vehicle = parse_vehicle(*veh_it);
        err = validate(*p.vehicle, p.tof);
        if (!err.empty()) throw std::runtime_error("params.json: " + err);
    }
    if (int_it != j.end()) {
        p.intersection = parse_intersection(*int_it);
        err = validate(*p.intersection);
        if (!err.empty()) throw std::runtime_error("params.json: " + err);
    }
    return p;
}

std::string cam_params_to_json(const CamParams& p) {
    json j = json::object();
    j["tof"] = json::parse(tof::params_to_json(p.tof));
    j["signal"] = {
        {"red_detector", p.signal.red_detector},
        {"green_detector", p.signal.green_detector},
        {"stop_min_cells", p.signal.stop_min_cells},
        {"approach_v", p.signal.approach_v},
        {"min_stop_ms", p.signal.min_stop_ms},
        {"resume_without_green_ms", p.signal.resume_without_green_ms},
        {"resume_ignore_ms", p.signal.resume_ignore_ms},
    };
    if (p.vehicle) {
        const VehicleParams& v = *p.vehicle;
        j["vehicle"] = {
            {"detector", v.detector},
            {"lane_cols", v.lane_cols},
            {"follow_gap_mm", v.follow_gap_mm},
            {"follow_band_mm", v.follow_band_mm},
            {"follow_v", v.follow_v},
            {"parked_confirm_ms", v.parked_confirm_ms},
            {"still_delta_mm", v.still_delta_mm},
            {"overtake_side", v.overtake_side},
            {"side_clear_mm", v.side_clear_mm},
            {"overtake_v", v.overtake_v},
            {"out_ms", v.out_ms},
            {"pass_min_ms", v.pass_min_ms},
            {"pass_max_ms", v.pass_max_ms},
            {"pass_clear_mm", v.pass_clear_mm},
            {"pass_clear_ms", v.pass_clear_ms},
            {"in_ms", v.in_ms},
            {"ignore_ms", v.ignore_ms},
        };
    }
    if (p.intersection) {
        const IntersectionParams& i = *p.intersection;
        j["intersection"] = {
            {"left_detector", i.left_detector},
            {"right_detector", i.right_detector},
            {"approach_v", i.approach_v},
            {"stop_trigger", i.stop_trigger},
            {"open_mm", i.open_mm},
            {"stop_ms", i.stop_ms},
            {"turn", i.turn},
            {"turn_v", i.turn_v},
            {"cross_max_ms", i.cross_max_ms},
            {"turn_ms", i.turn_ms},
            {"ignore_ms", i.ignore_ms},
        };
    }
    return j.dump(2) + "\n";
}

} // namespace rumicar::cam
