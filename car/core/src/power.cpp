#include "power.hpp"

#include <cctype>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <sstream>

namespace rumicar::power {

std::optional<uint32_t> parse_throttled(const std::string& text) {
    size_t i = 0;
    while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
    if (i + 1 < text.size() && text[i] == '0' && (text[i + 1] == 'x' || text[i + 1] == 'X')) i += 2;
    if (i >= text.size()) return std::nullopt;
    uint64_t value = 0;
    size_t digits = 0;
    for (; i < text.size(); ++i) {
        const char c = text[i];
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = 10 + (c - 'a');
        else if (c >= 'A' && c <= 'F') d = 10 + (c - 'A');
        else break;
        value = (value << 4) | static_cast<uint64_t>(d);
        if (++digits > 8) return std::nullopt;
    }
    if (digits == 0) return std::nullopt;
    for (; i < text.size(); ++i) {
        if (!std::isspace(static_cast<unsigned char>(text[i]))) return std::nullopt;
    }
    return static_cast<uint32_t>(value);
}

std::string describe_throttled(uint32_t flags) {
    if (flags == 0) return "ok";
    std::string s;
    auto add = [&](uint32_t bit, const char* name) {
        if (flags & bit) {
            if (!s.empty()) s += ", ";
            s += name;
        }
    };
    add(kUnderVoltageNow, "under-voltage now");
    add(kArmFrequencyCapped, "arm frequency capped");
    add(kThrottledNow, "throttled now");
    add(kSoftTemperatureLimit, "soft temperature limit");
    add(kUnderVoltageOccurred, "under-voltage since boot");
    add(kArmFrequencyCapOccurred, "arm frequency cap since boot");
    add(kThrottlingOccurred, "throttling since boot");
    add(kSoftTemperatureOccurred, "soft temperature limit since boot");
    if (flags & ~(kUnderVoltageNow | kArmFrequencyCapped | kThrottledNow | kSoftTemperatureLimit | kUnderVoltageOccurred |
                  kArmFrequencyCapOccurred | kThrottlingOccurred | kSoftTemperatureOccurred)) {
        if (!s.empty()) s += ", ";
        s += "unknown bits";
    }
    return s;
}

ThrottledMonitor::ThrottledMonitor(std::string path) : path_(std::move(path)) {
    if (path_.empty()) {
        reason_ = "disabled (power.throttled_path is empty)";
        return;
    }
    std::ifstream in(path_);
    if (!in) {
        reason_ = "cannot open " + path_ + ": " + std::strerror(errno);
        return;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    if (!parse_throttled(ss.str())) {
        reason_ = path_ + " does not contain a hexadecimal value";
        return;
    }
    available_ = true;
}

std::optional<uint32_t> ThrottledMonitor::read() const {
    if (!available_) return std::nullopt;
    std::ifstream in(path_);
    if (!in) return std::nullopt;
    std::stringstream ss;
    ss << in.rdbuf();
    return parse_throttled(ss.str());
}

} // namespace rumicar::power
