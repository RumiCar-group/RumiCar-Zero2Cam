// Process runner and Wi-Fi access-point request validation.
#include <chrono>
#include <string>

#include "auto_output.hpp"
#include "check.hpp"
#include "util/process.hpp"
#include "wifi.hpp"
#include "power.hpp"
#include "run_identity.hpp"
#include <sys/stat.h>
#include <unistd.h>
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

void test_auto_output_start_ramp() {
    // Switching to auto while the logic already asks for full speed: the drive
    // grows from 0 at the configured rate instead of jumping (docs 3.5).
    // Values on the RC_drive scale (duty 0..255); the direction is not the limiter's business.
    AutoOutputLimiter lim(287);
    int out = lim.drive(230, 255, 0.05);
    CHECK(out == 14);   // 287 * 0.05
    int prev = out;
    int frames = 1;
    while (out < 230 && frames < 100) {
        out = lim.drive(230, 255, 0.05);
        CHECK(out >= prev && out - prev <= 15);
        prev = out;
        ++frames;
    }
    CHECK(out == 230 && frames >= 15 && frames <= 17);   // 0.8 s
    CHECK(lim.drive(179, 255, 0.05) == 179);             // later the logic's own control is untouched
    CHECK(lim.drive(0, 255, 0.05) == 0);
    for (int i = 0; i < 20; ++i) lim.drive(230, 255, 0.05);
    CHECK(!lim.ramping());
    // Not driving (manual, record_only, e-stop, error): the next run starts from rest again.
    lim.idle();
    CHECK(lim.drive(230, 255, 0.05) == 14 && lim.ramping());
    // The auto limit caps at any time, also during the ramp.
    AutoOutputLimiter capped(287);
    for (int i = 0; i < 40; ++i) out = capped.drive(230, 102, 0.05);
    CHECK(out == 102);
    CHECK(capped.drive(77, 102, 0.05) == 77);
    // Without a ramp (0): only the limit.
    AutoOutputLimiter none(0);
    CHECK(none.drive(230, 255, 0.05) == 230 && none.drive(230, 153, 0.05) == 153);
    // A long pause between frames does not make a big step.
    AutoOutputLimiter pause(287);
    CHECK(pause.drive(230, 255, 5.0) == 57);   // dt clamped to 0.2 s
}

} // namespace

// The identity told to the PC in hello (docs 5.2).
void test_run_identity() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / ("rumicar-run-" + std::to_string(::getpid()));
    fs::remove_all(dir);
    const std::string marker = (dir / "status" / "core-running").string();
    {
        RunMarker first(marker);                       // nothing left over: the previous run ended cleanly
        CHECK(first.previous_exit() == "clean");
        CHECK(fs::exists(marker));                     // written (and flushed) while the core runs
    }
    CHECK(!fs::exists(marker));                        // removed by the core's own shutdown
    {
        RunMarker running(marker);
        // A second core started by hand while this one runs: not a cut run,
        // and the running core's marker stays.
        {
            RunMarker beside(marker);
            CHECK(beside.previous_exit() == "unknown");
        }
        CHECK(fs::exists(marker));
    }
    CHECK(!fs::exists(marker));
    {
        RunMarker second(marker);
        CHECK(second.previous_exit() == "clean");
        // A power loss, crash or SIGKILL ends the run without the destructor:
        // the file stays for the next start.
        fs::copy_file(marker, dir / "left", fs::copy_options::overwrite_existing);
    }
    fs::copy_file(dir / "left", marker, fs::copy_options::overwrite_existing);
    {
        RunMarker after_cut(marker);
        CHECK(after_cut.previous_exit() == "unclean");
    }
    CHECK(!fs::exists(marker));
    {
        // A marker that cannot be written: reported as unknown, the core runs on.
        RunMarker nowhere("/proc/rumicar-cannot-write/core-running");
        CHECK(nowhere.previous_exit() == "unknown");
    }

    std::ofstream(dir / "boot_id") << "0f1e2d3c-4b5a-6978-8796-a5b4c3d2e1f0\n";
    CHECK(read_boot_id((dir / "boot_id").string()) == "0f1e2d3c-4b5a-6978-8796-a5b4c3d2e1f0");
    CHECK(read_boot_id((dir / "missing").string()).empty());
    CHECK(read_boot_id().size() == 36);                // this machine's own boot id
    const std::string a = make_instance_id(), b = make_instance_id();
    CHECK(a.size() == 16 && a.find_first_not_of("0123456789abcdef") == std::string::npos);
    CHECK(a != b);

    // The access point's name from nmcli (-g escapes ':' and '\\'); a stand-in
    // nmcli that answers only the exact query.
    const fs::path fake = dir / "nmcli";
    std::ofstream(fake) << "#!/bin/sh\n"
                           "[ \"$*\" = \"-g 802-11-wireless.ssid connection show RumiCar-AP\" ] || exit 9\n"
                           "printf 'Rumi\\\\:Car 01\\n'\n";
    ::chmod(fake.c_str(), 0755);
    CHECK(WifiAp(fake.string(), "RumiCar-AP").ssid() == "Rumi:Car 01");
    CHECK(WifiAp(fake.string(), "Other-AP").ssid().empty());          // nmcli fails
    CHECK(WifiAp("/nonexistent/nmcli", "RumiCar-AP").ssid().empty());   // nmcli cannot run
    CHECK(WifiAp(fake.string(), "").ssid().empty());                   // no access point profile
    fs::remove_all(dir);
}

int main() {
    test_run_process();
    test_run_identity();
    test_wifi_validation();
    test_wifi_activation_and_tx_power();
    test_throttled_parsing();
    test_auto_output_start_ramp();
    return check::summary("test_util");
}
