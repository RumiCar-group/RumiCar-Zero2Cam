#include "wifi.hpp"

#include <chrono>
#include <stdexcept>
#include <thread>

#include "log.hpp"
#include "util/process.hpp"

namespace rumicar {

WifiAp::WifiAp(std::string nmcli_path, std::string connection)
    : nmcli_(std::move(nmcli_path)), connection_(std::move(connection)) {}

bool WifiAp::valid_ssid(const std::string& ssid) {
    if (ssid.empty() || ssid.size() > 32) return false;
    if (ssid.front() == ' ' || ssid.back() == ' ') return false;
    for (const char ch : ssid) {
        const auto c = static_cast<unsigned char>(ch);
        if (c < 0x20 || c > 0x7E) return false;   // printable ASCII only
    }
    return true;
}

void WifiAp::set(const std::string& ssid, int channel) {
    if (!available()) throw std::runtime_error("set_wifi is not configured on this car (network.ap_connection is empty)");
    if (!valid_ssid(ssid)) throw std::runtime_error("ssid must be 1..32 printable ASCII characters without leading/trailing spaces");
    if (channel < 1 || channel > 13) throw std::runtime_error("channel must be 1..13");

    // In-memory change only: the profile on disk (and therefore the next boot) keeps the defaults.
    const auto r = util::run_process({nmcli_, "connection", "modify", "--temporary", connection_,
                                      "802-11-wireless.ssid", ssid,
                                      "802-11-wireless.channel", std::to_string(channel)}, 5000);
    if (r.timed_out) throw std::runtime_error("nmcli did not finish within 5 s");
    if (r.exit_code != 0) throw std::runtime_error("nmcli connection modify failed: " + r.output);
    log::info("wifi: temporary SSID '" + ssid + "' channel " + std::to_string(channel) + " set, re-activating the access point");

    // Re-activate a moment later so the response to set_wifi is delivered first.
    std::thread([nmcli = nmcli_, connection = connection_] {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        const auto up = util::run_process({nmcli, "connection", "up", connection}, 20000);
        if (up.timed_out) log::error("wifi: nmcli connection up did not finish within 20 s");
        else if (up.exit_code != 0) log::error("wifi: nmcli connection up failed: " + up.output);
        else log::info("wifi: access point re-activated");
    }).detach();
}

bool WifiAp::activate(int timeout_ms) const {
    if (!available()) return false;
    if (is_active()) {
        log::info("wifi: access point " + connection_ + " is already active");
        return true;
    }
    const auto r = util::run_process({nmcli_, "connection", "up", connection_}, timeout_ms);
    if (r.timed_out) {
        log::error("wifi: nmcli connection up " + connection_ + " did not finish within " + std::to_string(timeout_ms) + " ms");
        return false;
    }
    if (r.exit_code != 0) {
        log::error("wifi: nmcli connection up " + connection_ + " failed: " + r.output);
        return false;
    }
    log::info("wifi: access point " + connection_ + " active");
    return true;
}

bool WifiAp::is_active() const {
    if (!available()) return false;
    const auto r = util::run_process({nmcli_, "-g", "GENERAL.STATE", "connection", "show", connection_}, 5000);
    if (r.timed_out || r.exit_code != 0) return false;
    return r.output.find("activated") != std::string::npos;
}

bool set_wifi_tx_power(const std::string& iw_path, const std::string& device, int dbm) {
    if (dbm < 1 || dbm > 30) {
        log::error("wifi: tx power " + std::to_string(dbm) + " dBm is out of range (1..30)");
        return false;
    }
    if (device.empty() || device.find('/') != std::string::npos || device.find(' ') != std::string::npos) {
        log::error("wifi: invalid device name for tx power");
        return false;
    }
    util::ProcessResult r;
    try {
        r = util::run_process({iw_path, "dev", device, "set", "txpower", "fixed", std::to_string(dbm * 100)}, 5000);
    } catch (const std::exception& e) {
        log::error(std::string("wifi: cannot run iw: ") + e.what());
        return false;
    }
    if (r.timed_out || r.exit_code != 0) {
        log::error("wifi: iw set txpower failed: " + (r.timed_out ? std::string("timeout") : r.output));
        return false;
    }
    log::info("wifi: tx power limited to " + std::to_string(dbm) + " dBm on " + device);
    return true;
}

} // namespace rumicar
