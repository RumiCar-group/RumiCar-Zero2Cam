// Core configuration (/etc/rumicar/core.json).  Strictly validated.
#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace rumicar {

struct CoreConfig {
    int schema = 1;

    // Identity reported in hello / telemetry
    std::string car_name = "RumiCar";

    // Driving mode at start-up: "record_only" (motors braked until the PC
    // selects auto) or "auto" (drives as soon as the logic is running).
    std::string startup_mode = "record_only";

    // Control loop
    int loop_period_ms = 50;         // target frame period
    int watchdog_ms = 200;           // motors are braked if the loop stalls longer than this
    int activation_poll_ms = 500;    // how often current/* links are checked for a change
    int max_drive_permille = 1000;   // global cap applied to the logic's drive command
    // Auto mode starts from rest: after any time the logic was not driving
    // (another mode, an emergency stop, an error) the drive allowed in auto
    // grows from 0 at this rate (per mille per second); 0 = no start ramp.
    int auto_accel_permille_per_s = 1125;

    // Paths
    std::string state_dir = "/var/lib/rumicar";

    // GPIO (BCM numbering on the Raspberry Pi header)
    std::string gpio_chip = "/dev/gpiochip0";
    unsigned steer_in1 = 17;         // DRV8835 AIN1
    unsigned steer_in2 = 27;         // DRV8835 AIN2
    std::array<unsigned, 3> tof_xshut{23, 24, 25}; // left, centre, right

    // Hardware PWM (sysfs), dtoverlay=pwm-2chan,pin=18,func=2,pin2=13,func2=4
    std::string pwm_chip = "/sys/class/pwm/pwmchip0";
    unsigned drive_in1 = 0;          // pwm0 -> GPIO18 -> DRV8835 BIN1
    unsigned drive_in2 = 1;          // pwm1 -> GPIO13 -> DRV8835 BIN2
    int pwm_frequency_hz = 960;
    int kick_permille = 784;         // start kick (200/255) ...
    int kick_ms = 20;                // ... for this long on a start/direction edge

    // I2C
    int i2c_bus = 1;
    std::array<uint8_t, 3> tof_addresses{0x2B, 0x2D, 0x2F}; // left, centre, right
    int tof_read_timeout_ms = 100;   // per sensor, per frame

    // Camera (docs 3.8).  enabled=false runs the car on the ToF sensors only.
    // on_demand=true keeps the camera off while nothing needs images (no PC
    // connected and not in auto mode) to save power; false runs it always.
    bool camera_enabled = true;
    bool camera_on_demand = true;
    unsigned camera_width = 320;
    unsigned camera_height = 240;
    int camera_fps = 20;
    int camera_jpeg_quality = 80;

    // Network (control and telemetry servers, docs 5.1)
    std::string bind_address = "0.0.0.0";
    int control_port = 5600;
    int telemetry_port = 5601;
    int heartbeat_timeout_ms = 1000;  // manual mode: stop when the PC's heartbeat is older than this
    int telemetry_queue_frames = 16;  // per client; older frames are dropped beyond this
    std::string ap_connection = "RumiCar-AP";   // NetworkManager connection id of the access point ("" = no set_wifi)
    std::string nmcli_path = "/usr/bin/nmcli";
    // Who activates the access point: "core" (this program, after the hardware
    // is up and before the camera starts, so the power-hungry parts come up one
    // at a time) or "boot" (NetworkManager's autoconnect; the core does nothing).
    std::string ap_start = "core";
    // Transmit power limit applied after the AP is up (`iw dev <wifi_device>
    // set txpower fixed`); 0 = leave the driver default.
    int wifi_txpower_dbm = 0;
    std::string wifi_device = "wlan0";
    std::string iw_path = "/usr/sbin/iw";

    // Power (docs 3.9): under-voltage / throttling flags from the firmware.
    // Empty path disables the monitor.
    std::string throttled_path = "/sys/devices/platform/soc/soc:firmware/get_throttled";
    int power_poll_ms = 1000;
};

// Parse core.json text.  Every key is optional (defaults above) but unknown
// keys and out-of-range values are rejected.  Throws std::runtime_error.
CoreConfig parse_core_config(const std::string& json_text);

// Load from a file.  Throws std::runtime_error.
CoreConfig load_core_config(const std::string& path);

} // namespace rumicar
