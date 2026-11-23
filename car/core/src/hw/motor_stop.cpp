#include "motor_stop.hpp"

#include <exception>
#include <filesystem>
#include <fstream>

#include "gpio.hpp"

namespace rumicar::hw {

namespace fs = std::filesystem;

std::vector<std::string> stop_motor_outputs(const MotorOutputs& out) {
    std::vector<std::string> problems;
    for (unsigned channel : out.drive_channels) {
        const std::string dir = out.pwm_chip + "/pwm" + std::to_string(channel);
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) continue;   // not exported: no output
        for (const char* file : {"duty_cycle", "enable"}) {   // the order of PwmChannel's destructor
            std::ofstream o(dir + "/" + file);
            o << 0;
            o.flush();
            if (!o) problems.push_back("cannot write 0 to " + dir + "/" + file);
        }
    }
    try {
        GpioOutputs lines(out.gpio_chip, out.steer_lines, "rumicar-stop-motors");   // requested low, released at the end of the scope
    } catch (const std::exception& e) {
        problems.push_back(std::string("steering lines: ") + e.what());
    }
    return problems;
}

} // namespace rumicar::hw
