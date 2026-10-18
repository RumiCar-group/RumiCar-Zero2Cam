// State shared between the control loop (main.cpp) and the network threads
// (control_server.cpp).  Every method takes the internal mutex for a very
// short time; nothing here blocks or performs I/O, so the control loop never
// waits on the network.
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

#include <nlohmann/json.hpp>

namespace rumicar {

enum class DriveMode { Auto, Manual, RecordOnly };

const char* drive_mode_name(DriveMode m);
bool parse_drive_mode(const std::string& s, DriveMode& out);

// What the control loop is running right now (for get_status / status events / telemetry).
struct RunningStatus {
    std::string logic_name;
    std::string logic_version;
    std::string pack_id;
    std::string pack_version;
    std::string state_name;   // logic state of the last frame
    uint8_t     state = 0;
    uint32_t    flags = 0;    // logic command flags of the last frame
};

struct ActivationResult {
    bool ok = false;
    std::string logic_dir;
    std::string pack_dir;
    std::string error;
};

class CarState {
public:
    using Clock = std::chrono::steady_clock;
    using EventSink = std::function<void(const std::string& event, const nlohmann::json& data)>;

    explicit CarState(int heartbeat_timeout_ms);

    // Receives "status", "estop" and "activation" events (called with no lock held).
    void set_event_sink(EventSink sink);

    // --- mode / e-stop / manual command (control server -> loop) -------------
    void set_mode(DriveMode m, Clock::time_point now);
    DriveMode mode() const;

    // Latch or clear the emergency stop.  Returns true when the state changed.
    bool set_estop(bool active, const std::string& reason);
    bool estop(std::string* reason = nullptr) const;

    // Returns false (and does nothing) unless the car is in manual mode.
    bool set_manual(int8_t steer, int drive_permille);
    void heartbeat(Clock::time_point now);

    void client_connected(bool connected);
    bool client_is_connected() const;

    // --- per-frame decision (loop) ------------------------------------------
    struct FrameInputs {
        DriveMode mode;
        bool estop;
        int8_t manual_steer;
        int manual_drive;
        bool manual_timed_out;   // manual mode and no heartbeat within the timeout
    };
    FrameInputs frame_inputs(Clock::time_point now) const;

    // --- running status (loop -> control server) -----------------------------
    void update_running(const RunningStatus& s);
    RunningStatus running() const;
    nlohmann::json status_json() const;   // get_status result / status event data
    void emit_status();                   // send a status event with the current state

    // --- activation hand-shake (control server -> loop -> control server) ----
    // The server swaps the current/* links, then calls request_activation()
    // and waits for the loop to verify and switch.
    uint64_t request_activation();                     // returns the generation to wait for
    bool take_activation_request();                    // loop: true once per request
    void complete_activation(const ActivationResult& r);   // loop
    // Waits until the generation `gen` has completed; false on timeout.
    bool wait_activation(uint64_t gen, std::chrono::milliseconds timeout, ActivationResult& out) const;

private:
    void emit(const std::string& event, const nlohmann::json& data) const;

    mutable std::mutex m_;
    mutable std::condition_variable cv_;
    EventSink sink_;
    const std::chrono::milliseconds heartbeat_timeout_;

    DriveMode mode_ = DriveMode::Auto;
    bool estop_ = false;
    std::string estop_reason_;
    int8_t manual_steer_ = 0;
    int manual_drive_ = 0;
    Clock::time_point last_heartbeat_{};
    bool client_connected_ = false;

    RunningStatus running_;

    uint64_t activation_requested_gen_ = 0;
    uint64_t activation_completed_gen_ = 0;
    bool activation_pending_ = false;
    ActivationResult last_activation_;
};

} // namespace rumicar
