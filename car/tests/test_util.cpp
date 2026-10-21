// Process runner and Wi-Fi access-point request validation.
#include <chrono>
#include <string>

#include "check.hpp"
#include "util/process.hpp"
#include "wifi.hpp"
#include "power.hpp"
#include <filesystem>
#include <fstream>

using namespace rumicar;

namespace {

void test_run_process() {
    auto r = util::run_process({"/bin/echo", "hello", "world"}, 2000);
    CHECK(r.exit_code == 0);
    CHECK(!r.timed_out);
    CHECK(r.output == "hello world\n");

    r = util::run_process({"/bin/sh", "-c", "echo out; echo err 1>&2; exit 3"}, 2000);
    CHECK(r.exit_code == 3);
    CHECK(r.output.find("out") != std::string::npos && r.output.find("err") != std::string::npos);

    const auto t0 = std::chrono::steady_clock::now();
    r = util::run_process({"/bin/sleep", "5"}, 200);
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    CHECK(r.timed_out);
    CHECK(r.exit_code == -1);
    CHECK(elapsed < std::chrono::seconds(2));

    r = util::run_process({"/nonexistent/program"}, 2000);
    CHECK(r.exit_code == 127);
    CHECK(r.output.find("exec failed") != std::string::npos);
}

void test_wifi_validation() {
    CHECK(WifiAp::valid_ssid("RumiCar"));
    CHECK(WifiAp::valid_ssid("Rumi Car 01"));
    CHECK(WifiAp::valid_ssid(std::string(32, 'a')));
    CHECK(!WifiAp::valid_ssid(""));
    CHECK(!WifiAp::valid_ssid(std::string(33, 'a')));
    CHECK(!WifiAp::valid_ssid(" lead"));
    CHECK(!WifiAp::valid_ssid("trail "));
    CHECK(!WifiAp::valid_ssid("tab\tin"));
    CHECK(!WifiAp::valid_ssid("\xE3\x81\x82"));   // non-ASCII

    WifiAp none("/usr/bin/nmcli", "");
    CHECK(!none.available());
    bool threw = false;
    try { none.set("X", 6); } catch (const std::exception& e) { threw = std::string(e.what()).find("not configured") != std::string::npos; }
    CHECK(threw);

    // nmcli that cannot be executed: the failure is reported, nothing is scheduled.
    WifiAp broken("/nonexistent/nmcli", "RumiCar-AP");
    CHECK(broken.available());
    threw = false;
    try { broken.set("RumiCar", 6); } catch (const std::exception& e) { threw = std::string(e.what()).find("nmcli connection modify failed") != std::string::npos; }
    CHECK(threw);
    threw = false;
    try { broken.set("RumiCar", 0); } catch (const std::exception& e) { threw = std::string(e.what()).find("channel") != std::string::npos; }
    CHECK(threw);
    threw = false;
    try { broken.set("", 6); } catch (const std::exception& e) { threw = std::string(e.what()).find("ssid") != std::string::npos; }
    CHECK(threw);
}

void test_wifi_activation_and_tx_power() {
    // No profile: nothing to activate.
    WifiAp none("/usr/bin/nmcli", "");
    CHECK(!none.activate(1000));
    CHECK(!none.is_active());
    // nmcli that cannot run: reported as not active / not activated, no exception.
    WifiAp broken("/nonexistent/nmcli", "RumiCar-AP");
    CHECK(!broken.is_active());
    CHECK(!broken.activate(1000));
    // Argument checks happen before anything is executed.
    CHECK(!set_wifi_tx_power("/nonexistent/iw", "wlan0", 0));
    CHECK(!set_wifi_tx_power("/nonexistent/iw", "wlan0", 31));
    CHECK(!set_wifi_tx_power("/nonexistent/iw", "wl an0", 10));
    CHECK(!set_wifi_tx_power("/nonexistent/iw", "", 10));
    // A missing iw is a logged failure, not an exception.
    CHECK(!set_wifi_tx_power("/nonexistent/iw", "wlan0", 10));
}

void test_throttled_parsing() {
    using namespace rumicar::power;
    CHECK(parse_throttled("0\n") == 0u);
    CHECK(parse_throttled("50000\n") == 0x50000u);
    CHECK(parse_throttled("0x50005") == 0x50005u);
    CHECK(parse_throttled("  1  ") == 1u);
    CHECK(!parse_throttled(""));
    CHECK(!parse_throttled("zz"));
    CHECK(!parse_throttled("0x"));
    CHECK(!parse_throttled("123456789"));   // more than 32 bits
    CHECK(!parse_throttled("5 x"));
    CHECK(describe_throttled(0) == "ok");
    CHECK(describe_throttled(kUnderVoltageNow) == "under-voltage now");
    CHECK(describe_throttled(kUnderVoltageNow | kUnderVoltageOccurred) == "under-voltage now, under-voltage since boot");
    CHECK(describe_throttled(0x100000u) == "unknown bits");

    ThrottledMonitor off("");
    CHECK(!off.available() && !off.read());
    ThrottledMonitor missing("/nonexistent/get_throttled");
    CHECK(!missing.available() && missing.reason().find("cannot open") != std::string::npos);

    const auto dir = std::filesystem::temp_directory_path() / "rumicar-test-util";
    std::filesystem::create_directories(dir);
    const auto file = dir / "get_throttled";
    { std::ofstream out(file); out << "50000\n"; }
    ThrottledMonitor mon(file.string());
    CHECK(mon.available());
    CHECK(mon.read() == 0x50000u);
    { std::ofstream out(file); out << "1\n"; }
    CHECK(mon.read() == 1u);
    { std::ofstream out(file); out << "garbage\n"; }
    ThrottledMonitor bad(file.string());
    CHECK(!bad.available());
}

} // namespace

int main() {
    test_run_process();
    test_wifi_validation();
    test_wifi_activation_and_tx_power();
    test_throttled_parsing();
    return check::summary("test_util");
}
