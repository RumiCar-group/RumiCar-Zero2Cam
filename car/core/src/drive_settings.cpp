#include "drive_settings.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include "rumicar/logic_api.h"
#include "util/atomic_file.hpp"

namespace rumicar {

using json = nlohmann::json;

namespace {

constexpr int kSchema = 1;

int checked_int(const json& j, const std::string& key, int lo, int hi) {
    if (!j.is_number_integer()) throw std::invalid_argument("'" + key + "' must be an integer");
    const auto v = j.get<long long>();
    if (v < lo || v > hi)
        throw std::invalid_argument("'" + key + "' must be in [" + std::to_string(lo) + ", " + std::to_string(hi) + "]");
    return static_cast<int>(v);
}

} // namespace

json drive_settings_json(const DriveSettings& s) {
    return json{
        {"manual_max_pwm", s.manual_max_pwm},
        {"auto_limit", s.auto_limit},
        {"manual_accel_pwm_per_s", s.manual_accel_pwm_per_s},
        {"pad_steer_strength", s.pad_steer_strength},
    };
}

DriveSettings apply_drive_settings(const DriveSettings& s, const json& changes) {
    if (!changes.is_object() || changes.empty()) throw std::invalid_argument("no setting given");
    DriveSettings out = s;
    for (const auto& [key, value] : changes.items()) {
        if (key == "manual_max_pwm") {
            out.manual_max_pwm = checked_int(value, key, DriveSettings::kManualMaxMin, RUMI_PWM_MAX);
        } else if (key == "auto_limit") {
            out.auto_limit = checked_int(value, key, 0, RUMI_PWM_MAX);
        } else if (key == "manual_accel_pwm_per_s") {
            out.manual_accel_pwm_per_s = checked_int(value, key, 0, DriveSettings::kAccelMax);
        } else if (key == "pad_steer_strength") {
            if (!value.is_boolean()) throw std::invalid_argument("'" + key + "' must be true or false");
            out.pad_steer_strength = value.get<bool>();
        } else {
            throw std::invalid_argument("unknown setting '" + key + "'");
        }
    }
    return out;
}

DriveSettingsFile::DriveSettingsFile(const std::string& state_dir)
    : path_((std::filesystem::path(state_dir) / "settings" / "drive.json").string()) {}

DriveSettings DriveSettingsFile::load(std::string& note) const {
    note.clear();
    std::error_code ec;
    if (!std::filesystem::exists(path_, ec)) return DriveSettings{};
    try {
        std::ifstream in(path_);
        if (!in) throw std::runtime_error("cannot open");
        std::stringstream ss;
        ss << in.rdbuf();
        json j = json::parse(ss.str());
        if (!j.is_object()) throw std::runtime_error("not a JSON object");
        const auto schema = j.find("schema");
        if (schema == j.end() || !schema->is_number_integer() || schema->get<long long>() != kSchema)
            throw std::runtime_error("schema must be " + std::to_string(kSchema));
        j.erase("schema");
        // A setting missing from the file (written by an earlier version) keeps its default.
        return j.empty() ? DriveSettings{} : apply_drive_settings(DriveSettings{}, j);
    } catch (const std::exception& e) {
        note = path_ + ": " + e.what() + "; using the defaults";
        return DriveSettings{};
    }
}

void DriveSettingsFile::save(const DriveSettings& s) const {
    json j = drive_settings_json(s);
    j["schema"] = kSchema;
    util::write_file_atomic(path_, j.dump(2) + "\n");
}

} // namespace rumicar
