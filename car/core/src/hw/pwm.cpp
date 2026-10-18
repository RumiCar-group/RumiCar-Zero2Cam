#include "pwm.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <thread>

namespace rumicar::hw {

namespace fs = std::filesystem;

PwmChannel::PwmChannel(const std::string& chip_path, unsigned channel, int frequency_hz) {
    if (frequency_hz <= 0) throw std::runtime_error("PwmChannel: frequency must be positive");
    dir_ = chip_path + "/pwm" + std::to_string(channel);

    std::error_code ec;
    if (!fs::is_directory(dir_, ec)) {
        std::ofstream exp(chip_path + "/export");
        if (!exp) throw std::runtime_error("PwmChannel: cannot open " + chip_path + "/export");
        exp << channel;
        exp.flush();
        if (!exp) throw std::runtime_error("PwmChannel: export of channel " + std::to_string(channel) + " failed");
        // The attribute files appear asynchronously after export.
        for (int i = 0; i < 50 && !fs::exists(dir_ + "/enable", ec); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (!fs::exists(dir_ + "/enable", ec)) throw std::runtime_error("PwmChannel: " + dir_ + " did not appear after export");
    }

    period_ns_ = 1'000'000'000L / frequency_hz;
    // Order matters: duty must never exceed the period, and the output must be
    // disabled while the period changes.
    write("enable", 0);
    write("duty_cycle", 0);
    write("period", period_ns_);
    write("enable", 1);
    last_permille_ = 0;
}

PwmChannel::~PwmChannel() {
    try {
        write("duty_cycle", 0);
        write("enable", 0);
    } catch (...) {
        // Destructors must not throw; the kernel keeps the last value.
    }
}

void PwmChannel::set_duty_permille(int permille) {
    permille = std::clamp(permille, 0, 1000);
    if (permille == last_permille_) return;
    write("duty_cycle", (period_ns_ * permille) / 1000);
    last_permille_ = permille;
}

void PwmChannel::write(const std::string& file, long value) const {
    std::ofstream out(dir_ + "/" + file);
    if (!out) throw std::runtime_error("PwmChannel: cannot open " + dir_ + "/" + file);
    out << value;
    out.flush();
    if (!out) throw std::runtime_error("PwmChannel: write to " + dir_ + "/" + file + " failed");
}

} // namespace rumicar::hw
