// rumicar-core — control loop of the RumiCar Zero 2 W camera model.
//
//   sensors (3x VL53L0X) + camera -> driving logic (plug-in) -> DRV8835 motors
//                                   |
//            control server (TCP 5600) / telemetry server (TCP 5601) <-> PC
//
// Responsibilities: hardware bring-up, running the active logic with the
// active pack, hot-swapping both when current/* links change, driving modes
// (auto / manual / record_only), emergency stop, braking on any stall
// (watchdog) or error, telemetry, and clean shutdown.  The car drives without
// any network: the servers are optional for the control loop.
//
// Start-up order (docs 3.9): the power-hungry parts are brought up one after
// another, each only once the previous one is settled, and the motors stay
// braked in record_only until the PC asks for auto (startup_mode):
//   motors braked -> ToF sensors -> logic/pack -> Wi-Fi access point
//   -> servers -> control loop; the camera only when images are needed.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "camera/camera_control.hpp"
#include "camera/image_source.hpp"
#include "car_state.hpp"
#include "config.hpp"
#include "hw/drv8835.hpp"
#include "hw/gpio.hpp"
#include "hw/i2c.hpp"
#include "hw/vl53l0x.hpp"
#include "log.hpp"
#include "logic_loader.hpp"
#include "net/control_server.hpp"
#include "net/telemetry_frame.hpp"
#include "net/telemetry_server.hpp"
#include "power.hpp"
#include "request_handler.hpp"
#include "rumicar/logic_api.h"
#include "version_store.hpp"
#include "wifi.hpp"

#ifndef RUMICAR_CORE_VERSION
#define RUMICAR_CORE_VERSION "0.0.0"
#endif

namespace {

using Clock = std::chrono::steady_clock;
using json = nlohmann::json;
using namespace std::chrono_literals;

std::atomic<bool> g_stop{false};

void on_signal(int) { g_stop.store(true); }

struct Active {
    std::shared_ptr<rumicar::LogicLibrary> lib;
    std::unique_ptr<rumicar::LogicInstance> instance;
    rumicar::ActiveSelection selection;
};

Active load_active(const rumicar::VersionStore& store, const rumicar::ActiveSelection& sel,
                   const std::shared_ptr<rumicar::LogicLibrary>& reuse_lib) {
    Active a;
    a.selection = sel;
    a.lib = (reuse_lib && reuse_lib->dir() == sel.logic_dir) ? reuse_lib
                                                             : std::make_shared<rumicar::LogicLibrary>(sel.logic_dir);
    a.instance = std::make_unique<rumicar::LogicInstance>(a.lib, sel.pack_dir);
    store.write_active(sel.logic_dir, a.lib->manifest().version, sel.pack_dir, a.instance->pack_manifest().version);
    return a;
}

void publish_running(rumicar::CarState& state, const Active& active, uint8_t logic_state, uint32_t flags) {
    rumicar::RunningStatus s;
    const rumicar::Manifest& lm = active.lib->manifest();
    const rumicar::Manifest& pm = active.instance->pack_manifest();
    s.logic_name = lm.id;
    s.logic_version = lm.version;
    s.logic_description = lm.description;
    s.pack_id = pm.id;
    s.pack_version = pm.version;
    s.pack_description = pm.description;
    s.pack_capabilities = pm.capabilities;
    s.pack_scenarios = pm.scenarios;
    s.pack_parent = pm.parent;
    s.state = logic_state;
    s.state_name = active.instance->state_name(logic_state);
    s.flags = flags;
    state.update_running(s);
}

int32_t to_tof_value(const rumicar::hw::Vl53l0x::Reading& r) {
    switch (r.status) {
    case rumicar::hw::Vl53l0x::Status::Ok:       return r.mm;
    case rumicar::hw::Vl53l0x::Status::NoTarget: return RUMI_TOF_NO_TARGET;
    case rumicar::hw::Vl53l0x::Status::Timeout:
    case rumicar::hw::Vl53l0x::Status::Error:    return RUMI_TOF_INVALID;
    }
    return RUMI_TOF_INVALID;
}

const char* status_name(rumicar::hw::Vl53l0x::Status s) {
    switch (s) {
    case rumicar::hw::Vl53l0x::Status::Ok:       return "ok";
    case rumicar::hw::Vl53l0x::Status::NoTarget: return "no-target";
    case rumicar::hw::Vl53l0x::Status::Timeout:  return "timeout";
    case rumicar::hw::Vl53l0x::Status::Error:    return "error";
    }
    return "?";
}

// Point current/* back at what is running after a failed activation, so that
// a reboot starts the working versions (docs 3.5).
void restore_links(const rumicar::VersionStore& store, const rumicar::ActiveSelection& running,
                   const rumicar::ActiveSelection& failed) {
    try {
        if (failed.logic_dir != running.logic_dir) store.restore_current("logic", running.logic_dir);
        if (failed.pack_dir != running.pack_dir) store.restore_current("pack", running.pack_dir);
        rumicar::log::info("current/* restored to the running versions");
    } catch (const std::exception& e) {
        rumicar::log::error(std::string("cannot restore current/* links: ") + e.what());
    }
}

} // namespace

int main(int argc, char** argv) {
    using namespace rumicar;

    const std::string config_path = argc > 1 ? argv[1] : "/etc/rumicar/core.json";
    CoreConfig cfg;
    try {
        if (std::filesystem::exists(config_path)) {
            cfg = load_core_config(config_path);
            log::info("rumicar-core " RUMICAR_CORE_VERSION " config " + config_path);
        } else {
            log::info("rumicar-core " RUMICAR_CORE_VERSION " config " + config_path + " not found, using built-in defaults");
        }
    } catch (const std::exception& e) {
        log::error(e.what());
        return 2;
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGPIPE, SIG_IGN);

    // --- hardware bring-up ---------------------------------------------------
    std::unique_ptr<hw::GpioOutputs> gpio;
    std::unique_ptr<hw::Drv8835> motors;
    std::unique_ptr<hw::I2cBus> bus;
    std::vector<std::unique_ptr<hw::Vl53l0x>> sensors;
    try {
        std::vector<unsigned> lines{cfg.steer_in1, cfg.steer_in2, cfg.tof_xshut[0], cfg.tof_xshut[1], cfg.tof_xshut[2]};
        gpio = std::make_unique<hw::GpioOutputs>(cfg.gpio_chip, lines, "rumicar-core");
        motors = std::make_unique<hw::Drv8835>(*gpio, cfg.pwm_chip, cfg.pwm_frequency_hz,
                                               hw::Drv8835::Pins{cfg.steer_in1, cfg.steer_in2, cfg.drive_in1, cfg.drive_in2},
                                               hw::Drv8835::Kick{cfg.kick_permille, cfg.kick_ms});
        motors->stop();
        bus = std::make_unique<hw::I2cBus>(cfg.i2c_bus);
        static const char* names[3] = {"tof-left", "tof-centre", "tof-right"};
        for (size_t i = 0; i < 3; ++i) {
            sensors.push_back(std::make_unique<hw::Vl53l0x>(*bus, *gpio, cfg.tof_xshut[i], cfg.tof_addresses[i], names[i]));
        }
        std::this_thread::sleep_for(50ms); // all sensors held in reset
        for (auto& s : sensors) {
            s->init();
            log::info(s->label() + " ready");
        }
    } catch (const std::exception& e) {
        log::error(std::string("hardware bring-up failed: ") + e.what());
        return 3;
    }

    // --- logic + pack --------------------------------------------------------
    VersionStore store(cfg.state_dir);
    DriveMode startup_mode = DriveMode::RecordOnly;
    parse_drive_mode(cfg.startup_mode, startup_mode);   // validated by the config parser
    CarState state(cfg.heartbeat_timeout_ms, startup_mode);
    log::info(std::string("start-up mode ") + drive_mode_name(startup_mode) +
              (startup_mode == DriveMode::RecordOnly ? " (motors braked until the PC selects auto)" : " (drives as soon as the logic runs)"));
    Active active;
    try {
        std::string reason;
        auto sel = store.read_current(reason);
        if (!sel) throw std::runtime_error("no active selection: " + reason);
        try {
            active = load_active(store, *sel, nullptr);
        } catch (const std::exception& e) {
            // Fall back to the previous selection once, so a bad activation
            // left behind (e.g. over SSH) does not keep the car from driving.
            ActiveSelection prev{store.resolve_link("previous", "logic"), store.resolve_link("previous", "pack")};
            if (prev.logic_dir.empty()) prev.logic_dir = sel->logic_dir;
            if (prev.pack_dir.empty()) prev.pack_dir = sel->pack_dir;
            if (prev == *sel) throw;
            log::error(std::string("cannot start the current selection: ") + e.what());
            log::warn("trying the previous selection: logic " + prev.logic_dir + ", pack " + prev.pack_dir);
            active = load_active(store, prev, nullptr);
            store.write_activation_result(false, *sel, e.what());
            restore_links(store, prev, *sel);
            sel = prev;
        }
        store.write_activation_result(true, *sel, "");
        log::info("logic " + active.lib->manifest().id + " " + active.lib->manifest().version + " from " + sel->logic_dir);
        log::info("pack " + active.instance->pack_manifest().id + " " + active.instance->pack_manifest().version + " from " +
                  sel->pack_dir);
    } catch (const std::exception& e) {
        log::error(std::string("cannot start driving logic: ") + e.what());
        return 4;
    }
    publish_running(state, active, 0, 0);

    // --- Wi-Fi access point (docs 3.9): brought up here, after the sensors and
    // with the motors braked, so its start-up current does not coincide with
    // anything else.  Failure is logged; rumicar-ap.timer retries on its own.
    WifiAp wifi(cfg.nmcli_path, cfg.ap_connection);
    if (cfg.ap_start == "core") {
        if (!wifi.available()) {
            log::info("wifi: no access point profile configured (network.ap_connection is empty)");
        } else {
            log::info("wifi: activating the access point " + cfg.ap_connection);
            const bool up = wifi.activate(20000);
            if (up && cfg.wifi_txpower_dbm > 0) set_wifi_tx_power(cfg.iw_path, cfg.wifi_device, cfg.wifi_txpower_dbm);
        }
    } else {
        log::info("wifi: access point left to NetworkManager (network.ap_start = boot)");
    }

    // --- camera (optional: the car drives on the ToF sensors without it) -------
    // The controller starts/stops the camera on a worker thread; with
    // camera.on_demand the camera runs only while a PC is connected or the
    // mode is auto, otherwise it runs all the time.
    std::unique_ptr<cam::CameraController> camera;
    if (cfg.camera_enabled) {
        camera = std::make_unique<cam::CameraController>(
            cam::make_image_source(cam::CameraConfig{cfg.camera_width, cfg.camera_height, cfg.camera_fps, cfg.camera_jpeg_quality}),
            30s);
        if (!cfg.camera_on_demand) camera->set_wanted(true);
        log::info(std::string("camera ") + (cfg.camera_on_demand ? "on demand (PC connected or auto mode)" : "always on"));
    } else {
        log::info("camera disabled in the configuration");
    }

    // --- power monitor (docs 3.9) ------------------------------------------------
    power::ThrottledMonitor throttled(cfg.throttled_path);
    if (throttled.available()) {
        log::info("power: monitoring " + cfg.throttled_path + " every " + std::to_string(cfg.power_poll_ms) + " ms");
    } else {
        log::info("power: throttling monitor off: " + throttled.reason());
    }
    auto next_power_poll = Clock::now();

    // --- network (optional: the car drives without it) -------------------------
    RequestHandler handler(state, store, wifi, cfg.car_name, RUMICAR_CORE_VERSION);
    net::TelemetryServer telemetry(cfg.bind_address, static_cast<uint16_t>(cfg.telemetry_port),
                                   static_cast<size_t>(cfg.telemetry_queue_frames));
    net::ControlServer control(
        cfg.bind_address, static_cast<uint16_t>(cfg.control_port),
        [&](const std::string& type, const json& params) { return handler.handle(type, params); },
        [&](bool connected, const std::string&) {
            state.client_connected(connected);
            if (!connected) handler.on_client_disconnected();
            if (!connected && state.mode() == DriveMode::Manual) {
                if (state.set_estop(true, "disconnected")) log::warn("PC disconnected in manual mode: emergency stop");
            }
        });
    state.set_event_sink([&](const std::string& event, const json& data) { control.push_event(event, data); });
    bool network_up = false;
    try {
        telemetry.start();
        control.start();
        network_up = true;
    } catch (const std::exception& e) {
        log::error(std::string("network servers not started, driving without the PC: ") + e.what());
    }

    // --- watchdog: brake if the control loop stalls --------------------------
    std::atomic<int64_t> last_tick_ns{Clock::now().time_since_epoch().count()};
    std::atomic<bool> watchdog_quit{false};
    std::thread watchdog([&] {
        bool tripped = false;
        while (!watchdog_quit.load()) {
            std::this_thread::sleep_for(20ms);
            const auto age = Clock::now() - Clock::time_point(Clock::duration(last_tick_ns.load()));
            if (age > std::chrono::milliseconds(cfg.watchdog_ms)) {
                if (!tripped) {
                    log::error("watchdog: control loop stalled, braking");
                    tripped = true;
                }
                try { motors->stop(); } catch (const std::exception& e) { log::error(std::string("watchdog stop failed: ") + e.what()); }
            } else if (tripped) {
                log::info("watchdog: control loop resumed");
                tripped = false;
            }
        }
    });

    // --- control loop --------------------------------------------------------
    const auto period = std::chrono::milliseconds(cfg.loop_period_ms);
    auto next = Clock::now();
    auto next_poll = Clock::now();
    uint32_t seq = 0;
    uint8_t last_state = 0xFF;
    int last_step_rc = 0;
    std::array<hw::Vl53l0x::Status, 3> last_status{hw::Vl53l0x::Status::Ok, hw::Vl53l0x::Status::Ok, hw::Vl53l0x::Status::Ok};
    ActiveSelection last_failed_selection;
    bool have_failed_selection = false;
    int overruns = 0;
    const std::vector<uint8_t> no_image;
    uint32_t last_image_seq = 0;

    log::info("control loop started, period " + std::to_string(cfg.loop_period_ms) + " ms");
    while (!g_stop.load()) {
        const auto t0 = Clock::now();

        rumi_sensor_frame frame{};
        frame.size = sizeof(frame);
        frame.seq = seq++;
        frame.t_us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(t0.time_since_epoch()).count());
        for (size_t i = 0; i < 3; ++i) {
            const auto r = sensors[i]->read(cfg.tof_read_timeout_ms);
            frame.tof_mm[i] = to_tof_value(r);
            if (r.status != last_status[i]) {
                log::warn(sensors[i]->label() + ": " + status_name(last_status[i]) + " -> " + status_name(r.status));
                last_status[i] = r.status;
            }
        }

        // Camera on demand: images are needed while a PC is connected (to see
        // and record them) or in auto mode (for the logic).  JPEG encoding is
        // needed only while a telemetry client is receiving frames.
        const size_t telemetry_clients = network_up ? telemetry.client_count() : 0;
        const bool pc_connected = telemetry_clients > 0 || state.client_is_connected();
        if (camera && cfg.camera_on_demand) camera->set_wanted(pc_connected || state.mode() == DriveMode::Auto);
        if (camera) camera->set_jpeg(telemetry_clients > 0);

        // Newest camera image, attached only once (the camera may be slower than the loop).
        std::shared_ptr<const cam::Image> image = camera ? camera->latest() : nullptr;
        if (image && image->sequence != last_image_seq) {
            last_image_seq = image->sequence;
            frame.image = image->rgb.data();
            frame.image_width = image->width;
            frame.image_height = image->height;
            frame.image_stride = image->stride;
        } else {
            image.reset();
        }

        // The logic always runs (its state is reported even when it is not driving).
        rumi_command cmd{};
        const int rc = active.instance->step(frame, cmd);
        if (rc == 0 && cmd.state != last_state) {
            log::info(std::string("state ") + (last_state == 0xFF ? "-" : active.instance->state_name(last_state)) + " -> " +
                      active.instance->state_name(cmd.state));
            last_state = cmd.state;
        }
        if (rc != 0 && rc != last_step_rc) log::error("logic step failed with code " + std::to_string(rc) + ", motors braked");
        last_step_rc = rc;

        // --- what actually goes to the motors ---------------------------------
        CarState::FrameInputs in = state.frame_inputs(t0);
        if (in.manual_timed_out && !in.estop) {
            if (state.set_estop(true, "heartbeat")) log::warn("no heartbeat from the PC in manual mode: emergency stop");
            in.estop = true;
        }
        int8_t out_steer = RUMI_STEER_CENTER;
        int out_drive = 0;
        bool out_brake = true;
        if (in.estop) {
            // braked
        } else if (in.mode == DriveMode::Auto) {
            if (rc == 0) {
                out_steer = cmd.steer;
                out_drive = std::clamp<int>(cmd.drive_permille, -cfg.max_drive_permille, cfg.max_drive_permille);
                out_brake = (cmd.flags & (RUMI_CMD_FLAG_BRAKE | RUMI_CMD_FLAG_FAULT)) != 0;
            }
        } else if (in.mode == DriveMode::Manual) {
            out_steer = in.manual_steer;
            out_drive = std::clamp<int>(in.manual_drive, -cfg.max_drive_permille, cfg.max_drive_permille);
            out_brake = false;
        } // RecordOnly: braked
        try {
            motors->apply(out_steer, out_drive, out_brake, t0);
        } catch (const std::exception& e) {
            log::error(std::string("motor output failed: ") + e.what());
        }
        last_tick_ns.store(Clock::now().time_since_epoch().count());

        // --- power ------------------------------------------------------------
        if (throttled.available() && t0 >= next_power_poll) {
            next_power_poll = t0 + std::chrono::milliseconds(cfg.power_poll_ms);
            if (const auto v = throttled.read()) {
                const uint32_t before = state.throttled();
                if (state.update_throttled(*v)) {
                    const bool uv_now = (*v & power::kUnderVoltageNow) != 0;
                    const bool uv_before = (before & power::kUnderVoltageNow) != 0;
                    if (uv_now && !uv_before) log::warn("power: under-voltage detected (" + power::describe_throttled(*v) + ")");
                    else if (!uv_now && uv_before) log::info("power: under-voltage cleared (" + power::describe_throttled(*v) + ")");
                    else log::info("power: " + power::describe_throttled(*v));
                    state.emit_status();
                }
            }
        }

        // --- status + telemetry -------------------------------------------------
        publish_running(state, active, rc == 0 ? cmd.state : last_state, rc == 0 ? cmd.flags : 0u);
        if (network_up) {
            try {
                const json header = {
                    {"seq", frame.seq},
                    {"t_us", frame.t_us},
                    {"tof_mm", {frame.tof_mm[0], frame.tof_mm[1], frame.tof_mm[2]}},
                    {"steer", out_steer},
                    {"drive_permille", out_drive},
                    {"brake", out_brake},
                    {"state", rc == 0 ? cmd.state : last_state},
                    {"state_name", active.instance->state_name(rc == 0 ? cmd.state : last_state)},
                    {"flags", rc == 0 ? cmd.flags : 0u},
                    {"mode", drive_mode_name(in.mode)},
                    {"estop", in.estop},
                    {"logic_steer", rc == 0 ? cmd.steer : 0},
                    {"logic_drive_permille", rc == 0 ? cmd.drive_permille : 0},
                    {"logic_name", active.lib->manifest().id},
                    {"logic_version", active.lib->manifest().version},
                    {"pack_id", active.instance->pack_manifest().id},
                    {"pack_version", active.instance->pack_manifest().version},
                    {"battery_mv", 0},
                    {"throttled", state.throttled()},
                    {"image_w", image ? image->width : 0u},
                    {"image_h", image ? image->height : 0u},
                    {"image_seq", image ? image->sequence : 0u},
                    {"image_t_us", image ? image->sensor_t_us : 0ull},
                };
                telemetry.publish(std::make_shared<const std::vector<uint8_t>>(
                    net::encode_telemetry_frame(header.dump(), image ? image->jpeg : no_image)));
            } catch (const std::exception& e) {
                log::error(std::string("telemetry frame not sent: ") + e.what());
            }
        }

        // --- activation check ------------------------------------------------
        const bool requested = state.take_activation_request();
        if (requested) have_failed_selection = false;   // a new request may retry a selection that failed before
        if (requested || t0 >= next_poll) {
            next_poll = t0 + std::chrono::milliseconds(cfg.activation_poll_ms);
            std::string reason;
            auto sel = store.read_current(reason);
            if (!sel) {
                if (requested) state.complete_activation(ActivationResult{false, "", "", reason});
            } else if (*sel == active.selection) {
                if (requested) state.complete_activation(ActivationResult{true, sel->logic_dir, sel->pack_dir, ""});
            } else if (!(have_failed_selection && *sel == last_failed_selection)) {
                log::info("activation requested: logic " + sel->logic_dir + ", pack " + sel->pack_dir);
                try {
                    Active fresh = load_active(store, *sel, active.lib);
                    active = std::move(fresh); // old instance destroyed here; frame boundary
                    last_state = 0xFF;
                    have_failed_selection = false;
                    store.write_activation_result(true, *sel, "");
                    publish_running(state, active, 0, 0);
                    log::info("activated logic " + active.lib->manifest().id + " " + active.lib->manifest().version + ", pack " +
                              active.instance->pack_manifest().id + " " + active.instance->pack_manifest().version);
                    state.complete_activation(ActivationResult{true, sel->logic_dir, sel->pack_dir, ""});
                } catch (const std::exception& e) {
                    last_failed_selection = *sel;
                    have_failed_selection = true;
                    try { store.write_activation_result(false, *sel, e.what()); } catch (const std::exception&) {}
                    log::error(std::string("activation failed, keeping current versions: ") + e.what());
                    restore_links(store, active.selection, *sel);
                    state.complete_activation(ActivationResult{false, sel->logic_dir, sel->pack_dir, e.what()});
                }
            }
        }

        // --- pacing ----------------------------------------------------------
        next += period;
        const auto now = Clock::now();
        if (now > next + period) {
            if (++overruns % 20 == 1) log::warn("control loop overrun (" + std::to_string(overruns) + " so far)");
            next = now;
        } else {
            std::this_thread::sleep_until(next);
        }
    }

    log::info("stopping");
    watchdog_quit.store(true);
    watchdog.join();
    try { motors->stop(); } catch (const std::exception&) {}
    control.stop();
    telemetry.stop();
    if (camera) {
        camera->shutdown();
        camera.reset();
    }
    state.set_event_sink(nullptr);
    active.instance.reset();
    active.lib.reset();
    sensors.clear();
    motors.reset();
    gpio.reset();
    return 0;
}
