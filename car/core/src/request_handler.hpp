// Control-channel requests (docs 5.2) applied to the car state and the
// version store.  Runs on the control server's client thread.
#pragma once

#include <chrono>
#include <string>

#include <nlohmann/json.hpp>

#include "car_state.hpp"
#include "drive_settings.hpp"
#include "upload.hpp"
#include "version_store.hpp"
#include "wifi.hpp"

namespace rumicar {

class RequestHandler {
public:
    RequestHandler(CarState& state, const VersionStore& store, WifiAp& wifi, std::string car_name, std::string core_version,
                   std::chrono::milliseconds activation_wait = std::chrono::milliseconds(5000));

    // Returns the result (json null = none).  Throws net::RequestError for ok=false.
    nlohmann::json handle(const std::string& type, const nlohmann::json& params);

    // The control client went away: an unfinished upload is discarded.
    void on_client_disconnected();

    // Told to the PC in the result of hello (client_timeout_ms), so that the PC
    // uses the same time to tell that the connection is lost (0 = not told).
    void set_client_timeout_ms(int ms) { client_timeout_ms_ = ms; }

    // Told to the PC in the result of hello (docs 5.2): boot_id, instance_id
    // and previous_exit (run_identity.hpp), so that after a lost connection
    // the PC can tell whether the car or the core restarted.
    void set_run_identity(std::string boot_id, std::string instance_id, std::string previous_exit) {
        boot_id_ = std::move(boot_id);
        instance_id_ = std::move(instance_id);
        previous_exit_ = std::move(previous_exit);
    }

    // 3 = manual_drive carries RC_steer's strength (steer_pwm) and the drive
    // settings the car keeps (set_drive_settings, in place of set_auto_limit);
    // 2 = RC_steer / RC_drive values (core 0.4.x and 0.5.x).
    static constexpr int kProtocolVersion = 3;
    static constexpr int kMaxLogLines = 500;

private:
    nlohmann::json activation_outcome(const char* what);

    CarState& state_;
    const VersionStore& store_;
    WifiAp& wifi_;
    UploadManager uploads_;
    DriveSettingsFile settings_file_;
    std::string car_name_;
    std::string core_version_;
    std::chrono::milliseconds activation_wait_;
    int client_timeout_ms_ = 0;
    std::string boot_id_, instance_id_, previous_exit_;
};

} // namespace rumicar
