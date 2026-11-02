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

    // Duty as a share of full on [0, 255] (the RC_drive scale: 150 = 150/255
    // of the period).  Throws std::runtime_error on failure.
    void set_duty(int pwm);

private:
    void write(const std::string& file, long value) const;
    std::string dir_;
    long period_ns_ = 0;
    int last_pwm_ = -1;
};

} // namespace rumicar::hw
