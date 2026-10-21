// Power monitoring (docs 3.9): the firmware's throttling flags, read from
// sysfs (/sys/devices/platform/soc/soc:firmware/get_throttled, the same value
// `vcgencmd get_throttled` prints).  The core logs changes, reports the value
// in status replies and telemetry, and leaves the reaction to the person at
// the PC; nothing here touches the motors.
//
// Whether the Zero 2 W's firmware actually detects under-voltage has to be
// confirmed on the car (docs 8.3); on a board without the detector the value
// simply stays 0, and on a host without the file the monitor is disabled.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace rumicar::power {

// Bits of the throttled value (Raspberry Pi firmware).
constexpr uint32_t kUnderVoltageNow        = 0x1;
constexpr uint32_t kArmFrequencyCapped     = 0x2;
constexpr uint32_t kThrottledNow           = 0x4;
constexpr uint32_t kSoftTemperatureLimit   = 0x8;
constexpr uint32_t kUnderVoltageOccurred   = 0x10000;
constexpr uint32_t kArmFrequencyCapOccurred = 0x20000;
constexpr uint32_t kThrottlingOccurred     = 0x40000;
constexpr uint32_t kSoftTemperatureOccurred = 0x80000;

// Parses the sysfs text ("50000", "0x50000", with or without a newline).
// Returns nullopt for anything that is not a hexadecimal number.
std::optional<uint32_t> parse_throttled(const std::string& text);

// Human-readable summary for logs ("ok", "under-voltage now, ...").
std::string describe_throttled(uint32_t flags);

class ThrottledMonitor {
public:
    // path: the sysfs attribute; empty disables the monitor.
    explicit ThrottledMonitor(std::string path);

    // False when the monitor is disabled or the file could not be read at
    // construction (the reason is in `reason()`).
    bool available() const { return available_; }
    const std::string& reason() const { return reason_; }

    // Reads the current flags.  nullopt when the file cannot be read now.
    std::optional<uint32_t> read() const;

private:
    std::string path_;
    bool available_ = false;
    std::string reason_;
};

} // namespace rumicar::power
