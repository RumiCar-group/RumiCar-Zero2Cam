// Hardware PWM via the Linux sysfs interface (/sys/class/pwm/pwmchipN).
#pragma once

#include <string>

namespace rumicar::hw {

class PwmChannel {
public:
    // Exports the channel if needed, sets the frequency, duty 0 and enables it.
    // Throws std::runtime_error.
    PwmChannel(const std::string& chip_path, unsigned channel, int frequency_hz);
    ~PwmChannel();
    PwmChannel(const PwmChannel&) = delete;
    PwmChannel& operator=(const PwmChannel&) = delete;

    // Duty in per mille [0, 1000].  Throws std::runtime_error on failure.
    void set_duty_permille(int permille);

private:
    void write(const std::string& file, long value) const;
    std::string dir_;
    long period_ns_ = 0;
    int last_permille_ = -1;
};

} // namespace rumicar::hw
