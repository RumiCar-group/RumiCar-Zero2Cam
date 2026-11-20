// Drive settings the PC sets and the car keeps across a restart (docs 3.4):
// the caps of the drive duty in manual and in auto mode, the acceleration of
// manual driving, and whether the drive manager's pad gives the steering
// strength.  They are per car, because cars differ in their motor and gears
// (how much current a start draws) and in their steering.
//
// They live in <state_dir>/settings/drive.json, written atomically by the
// control server whenever the PC changes one (set_drive_settings).  A missing
// file gives the defaults; an unreadable or invalid one gives the defaults
// too (the car still starts) and is reported in the log.
#pragma once

#include <string>

#include <nlohmann/json.hpp>

namespace rumicar {

struct DriveSettings {
    // Cap of the drive duty in manual mode (RC_drive's value): the pad's edge,
    // the scroll and the arrow keys reach it, and the car clamps manual_drive
    // to it.  The drive manager's former default.
    int manual_max_pwm = 153;
    // Cap of the drive duty in auto mode.  Defaults to the maximum
    // (max_drive_pwm of core.json, which caps it at load).
    int auto_limit = 255;
    // Growth of the manual drive duty, in duty per second (0..255 scale):
    // a rising duty climbs at this rate, a falling one follows at once.  287
    // (0 to 255 in about 0.9 s) is the auto start ramp's default; 0 = none.
    int manual_accel_pwm_per_s = 287;
    // Whether the drive manager's pad gives RC_steer's strength from how far
    // to the side it is held (true) or steers at full strength (false).  Kept
    // by the car for the drive manager (it depends on the car's steering).
    bool pad_steer_strength = false;

    static constexpr int kManualMaxMin = 26;     // the drive manager's lowest manual cap (about a tenth)
    static constexpr int kAccelMax = 25500;      // as auto_accel_pwm_per_s in core.json

    bool operator==(const DriveSettings&) const = default;
};

// The settings as JSON (get_status keys, the result of set_drive_settings, the file).
nlohmann::json drive_settings_json(const DriveSettings& s);

// `s` with the keys present in `changes` applied.  Every key is checked
// (known, of its type, in its range) before anything is applied; throws
// std::invalid_argument naming the key otherwise.  An empty object is an error.
DriveSettings apply_drive_settings(const DriveSettings& s, const nlohmann::json& changes);

class DriveSettingsFile {
public:
    explicit DriveSettingsFile(const std::string& state_dir);

    // The saved settings.  A missing file gives the defaults (note empty);
    // an unreadable or invalid one gives the defaults with `note` saying why.
    DriveSettings load(std::string& note) const;
    // Throws std::runtime_error when the file cannot be written.
    void save(const DriveSettings& s) const;

    const std::string& path() const { return path_; }

private:
    std::string path_;
};

} // namespace rumicar
