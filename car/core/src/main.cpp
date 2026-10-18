// rumicar-core — control loop of the RumiCar Zero 2 W camera model.
//
//   sensors (3x VL53L0X) -> driving logic (plug-in) -> DRV8835 motors
//
// Responsibilities: hardware bring-up, running the active logic with the
// active pack, hot-swapping both when current/* links change, braking on
// any stall (watchdog) or error, and clean shutdown.
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "config.hpp"
#include "hw/drv8835.hpp"
#include "hw/gpio.hpp"
#include "hw/i2c.hpp"
#include "hw/vl53l0x.hpp"
#include "log.hpp"
#include "logic_loader.hpp"
#include "rumicar/logic_api.h"
#include "version_store.hpp"

#ifndef RUMICAR_CORE_VERSION
#define RUMICAR_CORE_VERSION "0.0.0"
#endif

namespace {

using Clock = std::chrono::steady_clock;
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
    Active active;
    try {
        std::string reason;
        auto sel = store.read_current(reason);
        if (!sel) throw std::runtime_error("no active selection: " + reason);
        active = load_active(store, *sel, nullptr);
        store.write_activation_result(true, *sel, "");
        log::info("logic " + active.lib->manifest().id + " " + active.lib->manifest().version + " from " + sel->logic_dir);
        log::info("pack " + active.instance->pack_manifest().id + " " + active.instance->pack_manifest().version + " from " +
                  sel->pack_dir);
    } catch (const std::exception& e) {
        log::error(std::string("cannot start driving logic: ") + e.what());
        return 4;
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

        rumi_command cmd{};
        const int rc = active.instance->step(frame, cmd);
        try {
            if (rc != 0) {
                motors->stop();
                if (rc != last_step_rc) log::error("logic step failed with code " + std::to_string(rc) + ", motors braked");
            } else {
                const int drive = std::clamp<int>(cmd.drive_permille, -cfg.max_drive_permille, cfg.max_drive_permille);
                const bool brake = (cmd.flags & (RUMI_CMD_FLAG_BRAKE | RUMI_CMD_FLAG_FAULT)) != 0;
                motors->apply(cmd.steer, drive, brake, t0);
                if (cmd.state != last_state) {
                    log::info(std::string("state ") + (last_state == 0xFF ? "-" : active.instance->state_name(last_state)) +
                              " -> " + active.instance->state_name(cmd.state));
                    last_state = cmd.state;
                }
            }
        } catch (const std::exception& e) {
            log::error(std::string("motor output failed: ") + e.what());
        }
        last_step_rc = rc;
        last_tick_ns.store(Clock::now().time_since_epoch().count());

        // --- activation check ------------------------------------------------
        if (t0 >= next_poll) {
            next_poll = t0 + std::chrono::milliseconds(cfg.activation_poll_ms);
            std::string reason;
            auto sel = store.read_current(reason);
            if (sel && *sel != active.selection && !(have_failed_selection && *sel == last_failed_selection)) {
                log::info("activation requested: logic " + sel->logic_dir + ", pack " + sel->pack_dir);
                try {
                    Active fresh = load_active(store, *sel, active.lib);
                    active = std::move(fresh); // old instance destroyed here; frame boundary
                    last_state = 0xFF;
                    have_failed_selection = false;
                    store.write_activation_result(true, *sel, "");
                    log::info("activated logic " + active.lib->manifest().id + " " + active.lib->manifest().version + ", pack " +
                              active.instance->pack_manifest().id + " " + active.instance->pack_manifest().version);
                } catch (const std::exception& e) {
                    last_failed_selection = *sel;
                    have_failed_selection = true;
                    try { store.write_activation_result(false, *sel, e.what()); } catch (const std::exception&) {}
                    log::error(std::string("activation failed, keeping current versions: ") + e.what());
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
    active.instance.reset();
    active.lib.reset();
    sensors.clear();
    motors.reset();
    gpio.reset();
    return 0;
}
