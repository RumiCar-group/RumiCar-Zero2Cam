// Tests of the stop after rumicar-core has ended (docs 3.5): a core killed
// while driving (SIGKILL: no destructors run) leaves the drive's hardware PWM
// at its last duty, and `rumicar-core --stop-motors core.json`, the command
// systemd runs after every stop of the service (ExecStopPost), turns it off.
// The PWM chip is a directory laid out as the kernel's sysfs PWM chip
// (pwmN/period, duty_cycle, enable), so the real Drv8835 and the real
// rumicar-core binary write exactly the files they write on the car.  There
// is no GPIO chip here, so the steering part reports its problem (exit 1)
// while the drive is still stopped.
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include <sys/wait.h>
#include <unistd.h>

#include "check.hpp"
#include "hw/drv8835.hpp"
#include "hw/motor_stop.hpp"
#include "rumicar/logic_api.h"

using namespace rumicar;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

constexpr int kPwmHz = 960;

// A directory laid out as /sys/class/pwm/pwmchip0 with the given channels exported.
fs::path make_pwm_chip(const std::string& name, std::initializer_list<const char*> channels) {
    const fs::path chip = fs::temp_directory_path() / ("rumicar-" + name + "-" + std::to_string(::getpid()));
    fs::remove_all(chip);
    fs::create_directories(chip);
    for (const char* ch : channels) {
        fs::create_directories(chip / ch);
        for (const char* f : {"period", "duty_cycle", "enable"}) std::ofstream(chip / ch / f) << 0;
    }
    std::ofstream(chip / "export") << "";
    return chip;
}

long read_value(const fs::path& file) {
    std::ifstream in(file);
    long v = -1;
    in >> v;
    return v;
}

std::string read_text(const fs::path& file) {
    std::ifstream in(file);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

// A child process drives forward at 150 with the real driver on `chip` and is
// killed with SIGKILL while driving, as a crashed or killed core.
void drive_and_get_killed(const fs::path& chip) {
    const pid_t pid = ::fork();
    if (pid == 0) {
        hw::Drv8835 motors([](unsigned, bool) {}, 400, chip.string(), kPwmHz, hw::Drv8835::Pins{0, 1}, hw::Drv8835::Kick{200, 20});
        motors.apply(RUMI_STEER_CENTER, 0, RUMI_DRIVE_FORWARD, 150, RUMI_PWM_MAX);
        std::this_thread::sleep_for(100ms);   // past the start kick
        ::raise(SIGKILL);
        std::_Exit(3);
    }
    int status = 0;
    ::waitpid(pid, &status, 0);
    CHECK_MSG(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL, "the child was not killed");
}

void check_left_running(const fs::path& chip) {
    // Forward, slow decay: BIN1 full, BIN2 = 255 - 150; both enabled.
    const long period = 1'000'000'000L / kPwmHz;
    CHECK(read_value(chip / "pwm0" / "enable") == 1);
    CHECK(read_value(chip / "pwm1" / "enable") == 1);
    CHECK(read_value(chip / "pwm0" / "duty_cycle") == period);
    CHECK(read_value(chip / "pwm1" / "duty_cycle") == period * 105 / 255);
}

void check_stopped(const fs::path& chip) {
    for (const char* ch : {"pwm0", "pwm1"}) {
        CHECK(read_value(chip / ch / "duty_cycle") == 0);
        CHECK(read_value(chip / ch / "enable") == 0);
    }
}

// The library function on its own.
void test_stop_motor_outputs() {
    const fs::path chip = make_pwm_chip("stop-lib", {"pwm0", "pwm1"});
    drive_and_get_killed(chip);
    check_left_running(chip);
    const auto problems = hw::stop_motor_outputs({chip.string(), {0, 1}, (chip / "no-gpiochip").string(), {17, 27}});
    check_stopped(chip);
    CHECK(problems.size() == 1 && problems[0].find("steering lines") == 0);

    // A channel that is not exported has no output: it is left alone (not exported by the stop).
    const fs::path one = make_pwm_chip("stop-one", {"pwm0"});
    std::ofstream(one / "pwm0" / "enable") << 1;
    std::ofstream(one / "pwm0" / "duty_cycle") << 500000;
    hw::stop_motor_outputs({one.string(), {0, 1}, (one / "no-gpiochip").string(), {17, 27}});
    CHECK(read_value(one / "pwm0" / "enable") == 0 && read_value(one / "pwm0" / "duty_cycle") == 0);
    CHECK(!fs::exists(one / "pwm1"));
    CHECK(read_text(one / "export").empty());
    fs::remove_all(chip);
    fs::remove_all(one);
}

// The command systemd runs (ExecStopPost), with a core.json that points at the chip.
void test_stop_motors_command(const std::string& core_binary) {
    const fs::path chip = make_pwm_chip("stop-cmd", {"pwm0", "pwm1"});
    const fs::path config = chip / "core.json";
    std::ofstream(config) << "{\"pwm\": {\"chip\": \"" << chip.string() << "\", \"drive_in1\": 0, \"drive_in2\": 1},"
                          << " \"gpio\": {\"chip\": \"" << (chip / "no-gpiochip").string() << "\"}}\n";
    drive_and_get_killed(chip);
    check_left_running(chip);
    const std::string command = "'" + core_binary + "' --stop-motors '" + config.string() + "' 2>'" + (chip / "log").string() + "'";
    const int status = std::system(command.c_str());
    CHECK_MSG(WIFEXITED(status) && WEXITSTATUS(status) == 1, "exit status " + std::to_string(status));   // 1: the steering lines (no GPIO chip here)
    check_stopped(chip);
    const std::string log = read_text(chip / "log");
    CHECK_MSG(log.find("stop-motors: steering lines") != std::string::npos, log);
    CHECK_MSG(log.find("stop-motors: cannot write") == std::string::npos, log);
    fs::remove_all(chip);
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: test_motor_stop <rumicar-core>\n");
        return 2;
    }
    test_stop_motor_outputs();
    test_stop_motors_command(argv[1]);
    return check::summary("test_motor_stop");
}
