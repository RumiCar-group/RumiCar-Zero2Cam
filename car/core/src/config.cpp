#include "config.hpp"

#include <nlohmann/json.hpp>

#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>

namespace rumicar {

using json = nlohmann::json;

namespace {

struct Section {
    const json& j;
    std::string name;
    std::set<std::string> seen;

    bool has(const char* key) { return j.find(key) != j.end(); }

    int get_int(const char* key, int current, int lo, int hi) {
        auto it = j.find(key);
        if (it == j.end()) return current;
        seen.insert(key);
        if (!it->is_number_integer()) throw std::runtime_error("core.json: " + name + "." + key + " must be an integer");
        const auto v = it->get<long long>();
        if (v < lo || v > hi)
            throw std::runtime_error("core.json: " + name + "." + key + " must be in [" + std::to_string(lo) + ", " +
                                     std::to_string(hi) + "]");
        return static_cast<int>(v);
    }

    std::string get_string(const char* key, const std::string& current) {
        auto it = j.find(key);
        if (it == j.end()) return current;
        seen.insert(key);
        if (!it->is_string() || it->get<std::string>().empty())
            throw std::runtime_error("core.json: " + name + "." + key + " must be a non-empty string");
        return it->get<std::string>();
    }

    template <typename T, size_t N>
    void get_array(const char* key, std::array<T, N>& current, long long lo, long long hi) {
        auto it = j.find(key);
        if (it == j.end()) return;
        seen.insert(key);
        if (!it->is_array() || it->size() != N)
            throw std::runtime_error("core.json: " + name + "." + key + " must be an array of " + std::to_string(N));
        for (size_t i = 0; i < N; ++i) {
            const json& e = (*it)[i];
            if (!e.is_number_integer()) throw std::runtime_error("core.json: " + name + "." + key + " elements must be integers");
            const auto v = e.get<long long>();
            if (v < lo || v > hi) throw std::runtime_error("core.json: " + name + "." + key + " element out of range");
            current[i] = static_cast<T>(v);
        }
    }

    void reject_unknown() {
        for (auto it = j.begin(); it != j.end(); ++it) {
            if (!seen.count(it.key())) throw std::runtime_error("core.json: unknown key " + name + "." + it.key());
        }
    }
};

const json& section(const json& root, const char* key, std::set<std::string>& seen_top) {
    static const json empty = json::object();
    auto it = root.find(key);
    if (it == root.end()) return empty;
    seen_top.insert(key);
    if (!it->is_object()) throw std::runtime_error(std::string("core.json: '") + key + "' must be an object");
    return *it;
}

} // namespace

CoreConfig parse_core_config(const std::string& json_text) {
    json root;
    try {
        root = json::parse(json_text);
    } catch (const json::exception& e) {
        throw std::runtime_error(std::string("core.json: parse error: ") + e.what());
    }
    if (!root.is_object()) throw std::runtime_error("core.json: top level must be an object");

    CoreConfig c;
    std::set<std::string> seen_top;

    {
        Section s{root, "", {}};
        c.schema = s.get_int("schema", c.schema, 1, 1);
        c.loop_period_ms = s.get_int("loop_period_ms", c.loop_period_ms, 10, 500);
        c.watchdog_ms = s.get_int("watchdog_ms", c.watchdog_ms, 50, 2000);
        c.activation_poll_ms = s.get_int("activation_poll_ms", c.activation_poll_ms, 100, 10000);
        c.max_drive_permille = s.get_int("max_drive_permille", c.max_drive_permille, 0, 1000);
        seen_top.insert(s.seen.begin(), s.seen.end());
    }
    {
        Section s{section(root, "paths", seen_top), "paths", {}};
        c.state_dir = s.get_string("state_dir", c.state_dir);
        s.reject_unknown();
    }
    {
        Section s{section(root, "gpio", seen_top), "gpio", {}};
        c.gpio_chip = s.get_string("chip", c.gpio_chip);
        c.steer_in1 = static_cast<unsigned>(s.get_int("steer_in1", static_cast<int>(c.steer_in1), 0, 63));
        c.steer_in2 = static_cast<unsigned>(s.get_int("steer_in2", static_cast<int>(c.steer_in2), 0, 63));
        s.get_array("tof_xshut", c.tof_xshut, 0, 63);
        s.reject_unknown();
    }
    {
        Section s{section(root, "pwm", seen_top), "pwm", {}};
        c.pwm_chip = s.get_string("chip", c.pwm_chip);
        c.drive_in1 = static_cast<unsigned>(s.get_int("drive_in1", static_cast<int>(c.drive_in1), 0, 7));
        c.drive_in2 = static_cast<unsigned>(s.get_int("drive_in2", static_cast<int>(c.drive_in2), 0, 7));
        c.pwm_frequency_hz = s.get_int("frequency_hz", c.pwm_frequency_hz, 20, 50000);
        c.kick_permille = s.get_int("kick_permille", c.kick_permille, 0, 1000);
        c.kick_ms = s.get_int("kick_ms", c.kick_ms, 0, 200);
        s.reject_unknown();
    }
    {
        Section s{section(root, "i2c", seen_top), "i2c", {}};
        c.i2c_bus = s.get_int("bus", c.i2c_bus, 0, 20);
        s.get_array("tof_addresses", c.tof_addresses, 0x08, 0x77);
        c.tof_read_timeout_ms = s.get_int("tof_read_timeout_ms", c.tof_read_timeout_ms, 20, 1000);
        s.reject_unknown();
    }
    for (auto it = root.begin(); it != root.end(); ++it) {
        if (!seen_top.count(it.key())) throw std::runtime_error("core.json: unknown key " + it.key());
    }

    if (c.steer_in1 == c.steer_in2) throw std::runtime_error("core.json: gpio.steer_in1 and steer_in2 must differ");
    if (c.drive_in1 == c.drive_in2) throw std::runtime_error("core.json: pwm.drive_in1 and drive_in2 must differ");
    for (size_t i = 0; i < 3; ++i) {
        for (size_t k = i + 1; k < 3; ++k) {
            if (c.tof_xshut[i] == c.tof_xshut[k]) throw std::runtime_error("core.json: gpio.tof_xshut must be distinct");
            if (c.tof_addresses[i] == c.tof_addresses[k]) throw std::runtime_error("core.json: i2c.tof_addresses must be distinct");
        }
        if (c.tof_xshut[i] == c.steer_in1 || c.tof_xshut[i] == c.steer_in2)
            throw std::runtime_error("core.json: gpio.tof_xshut overlaps the steering pins");
        if (c.tof_addresses[i] == 0x29) throw std::runtime_error("core.json: i2c.tof_addresses must not use the default 0x29");
    }
    if (c.watchdog_ms < 2 * c.loop_period_ms) throw std::runtime_error("core.json: watchdog_ms must be at least twice loop_period_ms");
    return c;
}

CoreConfig load_core_config(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open " + path);
    std::stringstream ss;
    ss << in.rdbuf();
    return parse_core_config(ss.str());
}

} // namespace rumicar
