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

} // namespace rumicar
