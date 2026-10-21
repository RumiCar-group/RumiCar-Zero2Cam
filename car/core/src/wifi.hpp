// Temporary changes to the car's Wi-Fi access point through NetworkManager
// (docs 5.1 / 5.2 set_wifi).  The AP itself is the connection profile
// installed by the package (deploy/RumiCar-AP.nmconnection); this class only
// modifies it in memory (nmcli --temporary) and re-activates it, so a reboot
// restores the SSID and channel from the profile on disk.
#pragma once

#include <string>

namespace rumicar {

class WifiAp {
public:
    // connection: NetworkManager connection id (empty = set_wifi unsupported).
    WifiAp(std::string nmcli_path, std::string connection);

    bool available() const { return !connection_.empty(); }
    const std::string& connection() const { return connection_; }

    // Validates (1..32 printable ASCII bytes, no leading/trailing space;
    // channel 1..13), applies the temporary change and schedules the
    // re-activation shortly after returning so the reply still reaches the
    // PC.  Throws std::runtime_error with a message for the PC on failure.
    void set(const std::string& ssid, int channel);

    static bool valid_ssid(const std::string& ssid);

    // Activates the access point profile (nmcli connection up) and waits for
    // it, at most timeout_ms.  Used at start-up when network.ap_start is
    // "core" (docs 3.9).  Returns false (and logs why) when it did not come up;
    // the rumicar-ap.timer unit keeps retrying independently of the core.
    bool activate(int timeout_ms) const;

    // True when NetworkManager reports the profile as activated.
    bool is_active() const;

private:
    std::string nmcli_;
    std::string connection_;
};

// Limits the transmit power of a wireless device (`iw dev <device> set
// txpower fixed <mBm>`), docs 3.9.  dbm 1..30.  Returns false and logs on
// failure (missing iw, driver refusal); the car keeps running either way.
bool set_wifi_tx_power(const std::string& iw_path, const std::string& device, int dbm);

} // namespace rumicar
