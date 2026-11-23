// Stops the motor outputs after rumicar-core has ended (docs 3.5): run by
// systemd after every stop of the service (ExecStopPost=rumicar-core
// --stop-motors), so also after a crash or a SIGKILL, when the core's own
// shutdown (Drv8835 and PwmChannel destructors) did not run.  The drive's
// hardware PWM (sysfs) keeps its last duty when the process that set it dies,
// and the steering lines keep their last level.
#pragma once

#include <string>
#include <vector>

namespace rumicar::hw {

struct MotorOutputs {
    std::string pwm_chip;                  // sysfs PWM chip of the drive (core.json pwm.chip)
    std::vector<unsigned> drive_channels;  // BIN1, BIN2 (pwm.drive_in1, pwm.drive_in2)
    std::string gpio_chip;                 // GPIO chip of the steering (gpio.chip)
    std::vector<unsigned> steer_lines;     // AIN1, AIN2 (gpio.steer_in1, gpio.steer_in2)
};

// Drive: each exported channel gets duty 0 and is disabled, as PwmChannel's
// destructor leaves it (a channel that is not exported has no output and is
// left alone).  Steering: the lines are requested as outputs at low (coast),
// as the core leaves them, and released.  Every step is tried; the problems
// are returned (empty: everything is stopped).
std::vector<std::string> stop_motor_outputs(const MotorOutputs& out);

} // namespace rumicar::hw
