// Process runner and Wi-Fi access-point request validation.
#include <chrono>
#include <string>

#include "check.hpp"
#include "util/process.hpp"
#include "wifi.hpp"

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

} // namespace

int main() {
    test_run_process();
    test_wifi_validation();
    return check::summary("test_util");
}
