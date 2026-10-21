#include "car_state.hpp"

namespace rumicar {

using json = nlohmann::json;

const char* drive_mode_name(DriveMode m) {
    switch (m) {
    case DriveMode::Auto:       return "auto";
    case DriveMode::Manual:     return "manual";
    case DriveMode::RecordOnly: return "record_only";
    }
    return "?";
}

bool parse_drive_mode(const std::string& s, DriveMode& out) {
    if (s == "auto")        { out = DriveMode::Auto; return true; }
    if (s == "manual")      { out = DriveMode::Manual; return true; }
    if (s == "record_only") { out = DriveMode::RecordOnly; return true; }
    return false;
}

CarState::CarState(int heartbeat_timeout_ms, DriveMode initial_mode)
    : heartbeat_timeout_(heartbeat_timeout_ms), mode_(initial_mode), startup_mode_(initial_mode) {}

void CarState::set_event_sink(EventSink sink) {
    std::lock_guard<std::mutex> lock(m_);
    sink_ = std::move(sink);
}

void CarState::emit(const std::string& event, const json& data) const {
    EventSink sink;
    {
        std::lock_guard<std::mutex> lock(m_);
        sink = sink_;
    }
    if (sink) sink(event, data);
}

// --- mode / e-stop / manual ---------------------------------------------------

void CarState::set_mode(DriveMode m, Clock::time_point now) {
    {
        std::lock_guard<std::mutex> lock(m_);
        mode_ = m;
        // Entering any mode resets the manual command; the set_mode request
        // itself counts as a heartbeat so manual driving can start at once.
        manual_steer_ = 0;
        manual_drive_ = 0;
        last_heartbeat_ = now;
    }
    emit_status();
}

DriveMode CarState::mode() const {
    std::lock_guard<std::mutex> lock(m_);
    return mode_;
}

bool CarState::set_estop(bool active, const std::string& reason) {
    {
        std::lock_guard<std::mutex> lock(m_);
        if (estop_ == active) return false;
        estop_ = active;
        estop_reason_ = active ? reason : "";
        manual_steer_ = 0;
        manual_drive_ = 0;
    }
    emit("estop", json{{"active", active}, {"reason", reason}});
    emit_status();
    return true;
}

bool CarState::estop(std::string* reason) const {
    std::lock_guard<std::mutex> lock(m_);
    if (reason) *reason = estop_reason_;
    return estop_;
}

bool CarState::set_manual(int8_t steer, int drive_permille) {
    std::lock_guard<std::mutex> lock(m_);
    if (mode_ != DriveMode::Manual) return false;
    manual_steer_ = steer;
    manual_drive_ = drive_permille;
    return true;
}

void CarState::heartbeat(Clock::time_point now) {
    std::lock_guard<std::mutex> lock(m_);
    last_heartbeat_ = now;
}

void CarState::client_connected(bool connected) {
    std::lock_guard<std::mutex> lock(m_);
    client_connected_ = connected;
}

bool CarState::client_is_connected() const {
    std::lock_guard<std::mutex> lock(m_);
    return client_connected_;
}

CarState::FrameInputs CarState::frame_inputs(Clock::time_point now) const {
    std::lock_guard<std::mutex> lock(m_);
    FrameInputs in;
    in.mode = mode_;
    in.estop = estop_;
    in.manual_steer = manual_steer_;
    in.manual_drive = manual_drive_;
    in.manual_timed_out = mode_ == DriveMode::Manual && (now - last_heartbeat_) > heartbeat_timeout_;
    return in;
}

// --- running status ---------------------------------------------------------

void CarState::update_running(const RunningStatus& s) {
    std::lock_guard<std::mutex> lock(m_);
    running_ = s;
}

RunningStatus CarState::running() const {
    std::lock_guard<std::mutex> lock(m_);
    return running_;
}

bool CarState::update_throttled(uint32_t flags) {
    std::lock_guard<std::mutex> lock(m_);
    if (throttled_ == flags) return false;
    throttled_ = flags;
    return true;
}

uint32_t CarState::throttled() const {
    std::lock_guard<std::mutex> lock(m_);
    return throttled_;
}

json CarState::status_json() const {
    std::lock_guard<std::mutex> lock(m_);
    return json{
        {"mode", drive_mode_name(mode_)},
        {"startup_mode", drive_mode_name(startup_mode_)},
        {"estop", estop_},
        {"estop_reason", estop_reason_},
        {"logic_name", running_.logic_name},
        {"logic_version", running_.logic_version},
        {"logic_description", running_.logic_description},
        {"pack_id", running_.pack_id},
        {"pack_version", running_.pack_version},
        {"pack_description", running_.pack_description},
        {"pack_capabilities", running_.pack_capabilities},
        {"pack_scenarios", running_.pack_scenarios},
        {"pack_parent", running_.pack_parent},
        {"state_name", running_.state_name},
        {"battery_mv", 0},
        {"throttled", throttled_},
    };
}

void CarState::emit_status() { emit("status", status_json()); }

// --- activation hand-shake ------------------------------------------------------

uint64_t CarState::request_activation() {
    std::lock_guard<std::mutex> lock(m_);
    activation_pending_ = true;
    return ++activation_requested_gen_;
}

bool CarState::take_activation_request() {
    std::lock_guard<std::mutex> lock(m_);
    const bool pending = activation_pending_;
    activation_pending_ = false;
    return pending;
}

void CarState::complete_activation(const ActivationResult& r) {
    {
        std::lock_guard<std::mutex> lock(m_);
        last_activation_ = r;
        activation_completed_gen_ = activation_requested_gen_;
    }
    cv_.notify_all();
    emit("activation", json{{"ok", r.ok}, {"logic_dir", r.logic_dir}, {"pack_dir", r.pack_dir}, {"error", r.error}});
    emit_status();
}

bool CarState::wait_activation(uint64_t gen, std::chrono::milliseconds timeout, ActivationResult& out) const {
    std::unique_lock<std::mutex> lock(m_);
    const bool done = cv_.wait_for(lock, timeout, [&] { return activation_completed_gen_ >= gen; });
    if (done) out = last_activation_;
    return done;
}

} // namespace rumicar
