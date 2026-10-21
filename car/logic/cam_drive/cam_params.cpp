#include "cam_params.hpp"

#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>

#include "params_json.hpp"
#include "vision_pack.hpp"

namespace rumicar::cam {

using json = nlohmann::json;

namespace {

int take_int(const json& j, std::set<std::string>& seen, const char* key, int lo, int hi) {
    const auto it = j.find(key);
    if (it == j.end()) throw std::runtime_error(std::string("params.json: signal: missing key '") + key + "'");
    if (!it->is_number_integer()) throw std::runtime_error(std::string("params.json: signal.") + key + " must be an integer");
    const auto v = it->get<long long>();
    if (v < lo || v > hi)
        throw std::runtime_error(std::string("params.json: signal.") + key + " must be in [" + std::to_string(lo) + ", " +
                                 std::to_string(hi) + "]");
    seen.insert(key);
    return static_cast<int>(v);
}

std::string take_name(const json& j, std::set<std::string>& seen, const char* key) {
    const auto it = j.find(key);
    if (it == j.end()) throw std::runtime_error(std::string("params.json: signal: missing key '") + key + "'");
    if (!it->is_string()) throw std::runtime_error(std::string("params.json: signal.") + key + " must be a string");
    seen.insert(key);
    return it->get<std::string>();
}

} // namespace

std::string validate(const SignalParams& p) {
    if (!p.red_detector.empty() && !vision::valid_detector_name(p.red_detector)) return "signal.red_detector is not a valid detector name";
    if (!p.green_detector.empty() && !vision::valid_detector_name(p.green_detector)) return "signal.green_detector is not a valid detector name";
    if (p.red_detector.empty() && !p.green_detector.empty()) return "signal.green_detector needs signal.red_detector";
    if (p.stop_min_cells < 1 || p.stop_min_cells > 4096) return "signal.stop_min_cells must be 1..4096";
    if (p.approach_v < 0 || p.approach_v > RUMI_DRIVE_MAX) return "signal.approach_v out of range";
    if (p.min_stop_ms < 0 || p.min_stop_ms > 60000) return "signal.min_stop_ms out of range";
    if (p.resume_without_green_ms < 0 || p.resume_without_green_ms > 600000) return "signal.resume_without_green_ms out of range";
    if (p.green_detector.empty() && p.resume_without_green_ms == 0) return "signal.resume_without_green_ms must be > 0 without a green detector";
    if (p.resume_ignore_ms < 0 || p.resume_ignore_ms > 60000) return "signal.resume_ignore_ms out of range";
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
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (it.key() != "tof" && it.key() != "signal") throw std::runtime_error("params.json: unknown key '" + it.key() + "'");
    }

    CamParams p;
    p.tof = tof::parse_params_json(tof_it->dump());

    std::set<std::string> seen;
    const json& s = *sig_it;
    p.signal.red_detector = take_name(s, seen, "red_detector");
    p.signal.green_detector = take_name(s, seen, "green_detector");
    p.signal.stop_min_cells = take_int(s, seen, "stop_min_cells", 1, 4096);
    p.signal.approach_v = take_int(s, seen, "approach_v", 0, RUMI_DRIVE_MAX);
    p.signal.min_stop_ms = take_int(s, seen, "min_stop_ms", 0, 60000);
    p.signal.resume_without_green_ms = take_int(s, seen, "resume_without_green_ms", 0, 600000);
    p.signal.resume_ignore_ms = take_int(s, seen, "resume_ignore_ms", 0, 60000);
    for (auto it = s.begin(); it != s.end(); ++it) {
        if (!seen.count(it.key())) throw std::runtime_error("params.json: unknown key 'signal." + it.key() + "'");
    }
    const std::string err = validate(p.signal);
    if (!err.empty()) throw std::runtime_error("params.json: " + err);
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
    return j.dump(2) + "\n";
}

} // namespace rumicar::cam
