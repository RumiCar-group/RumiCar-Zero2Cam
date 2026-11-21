// Tests of py_drive, the logic that runs the user's own Python program
// (drive.py), through the real plug-in, runner.py and the RumiCar.py the
// build took from the RumiCar repository: the sample pack, RC_read with and
// without a limit (RC_setReadLimit), RC_steer's strength, which library
// drive.py gets, errors while loading (syntax, missing loop, params.json), a
// program that raises, one that hangs, one that exits, wrong values, print(),
// and that the control loop never waits longer than the step timeout.
// Usage: test_py_drive <staged py_drive logic dir> <py-sample pack dir>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include <dlfcn.h>
#include <signal.h>
#include <unistd.h>

#include "check.hpp"
#include "rumicar/logic_api.h"

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

const rumi_logic_api* api = nullptr;
fs::path tmp;

void write_file(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << s;
}

fs::path make_pack(const std::string& name, const std::string& drive_py, const std::string& params = "{}") {
    const fs::path dir = tmp / name;
    write_file(dir / "drive.py", drive_py);
    if (!params.empty()) write_file(dir / "params.json", params);
    return dir;
}

struct Run {
    void* inst = nullptr;
    std::string error;
    uint32_t seq = 0;
    uint64_t t_us = 1'000'000;
    rumi_command out{};
    explicit Run(const fs::path& pack) {
        char err[512] = {};
        inst = api->create(pack.c_str(), err, sizeof err);
        error = err;
    }
    ~Run() { if (inst) api->destroy(inst); }
    int step(int L, int C, int R) {
        rumi_sensor_frame f{};
        f.size = sizeof f;
        f.seq = seq++;
        f.t_us = t_us;
        t_us += 50'000;
        f.tof_mm[0] = L; f.tof_mm[1] = C; f.tof_mm[2] = R;
        return api->step(inst, &f, &out);
    }
    std::string state() const { return api->state_name(inst, out.state); }
};

void test_api() {
    CHECK(api->abi_version == RUMI_LOGIC_ABI_VERSION);
    CHECK(std::string(api->name) == "py_drive");
    CHECK(api->aux_name(nullptr, 0) == nullptr);
}

void test_sample_pack(const fs::path& sample) {
    Run r(sample);
    CHECK_MSG(r.inst != nullptr, r.error);
    if (!r.inst) return;
    char err[256] = {};
    CHECK(api->self_test(r.inst, err, sizeof err) == 0);
    // Open road (out of range, -3): straight at "speed" (153).
    CHECK(r.step(RUMI_TOF_OUT_OF_RANGE, RUMI_TOF_OUT_OF_RANGE, RUMI_TOF_OUT_OF_RANGE) == 0);
    CHECK(r.out.size == sizeof(rumi_command));
    CHECK(r.out.steer == RUMI_STEER_CENTER && r.out.steer_pwm == 0 && r.out.drive_dir == RUMI_DRIVE_FORWARD && r.out.drive_pwm == 153 && r.out.flags == 0);
    CHECK(r.state() == "RUN");
    // The sample sets read_limit_mm (200) of its params.json with RC_setReadLimit:
    // a distance beyond it is far, exactly as -3.  Walls 1.5 m ahead and 900 / 201 mm
    // to the sides: straight.
    CHECK(r.step(900, 1500, 201) == 0);
    CHECK(r.out.steer == RUMI_STEER_CENTER && r.out.steer_pwm == 0 && r.out.drive_dir == RUMI_DRIVE_FORWARD && r.out.drive_pwm == 153 && r.state() == "RUN");
    // Something 150 mm ahead, nothing on the left, a wall 900 mm away on the
    // right: both sides are far, so the turn goes right, as with -3 on both
    // sides.  Without the limit the 900 would lose against "far" and the car
    // would turn left.
    CHECK(r.step(RUMI_TOF_OUT_OF_RANGE, 150, 900) == 0);
    CHECK(r.out.steer == RUMI_STEER_RIGHT && r.out.drive_dir == RUMI_DRIVE_FORWARD && r.out.drive_pwm == 115 && r.state() == "TURN");
    // The limit itself is still a distance: 200 mm ahead is not below turn_mm (200) -> straight; 199 turns.
    CHECK(r.step(900, 200, 201) == 0 && r.state() == "RUN");
    CHECK(r.step(900, 199, 201) == 0 && r.state() == "TURN" && r.out.drive_pwm == 115);
    CHECK(r.step(RUMI_TOF_OUT_OF_RANGE, RUMI_TOF_OUT_OF_RANGE, RUMI_TOF_OUT_OF_RANGE) == 0 && r.state() == "RUN");
    // A wall near on the left: steer right.
    CHECK(r.step(40, RUMI_TOF_OUT_OF_RANGE, RUMI_TOF_OUT_OF_RANGE) == 0);
    CHECK(r.out.steer == RUMI_STEER_RIGHT && r.out.steer_pwm == 255 && r.state() == "RUN");
    // Something ahead, more room on the left: turn left slowly.
    CHECK(r.step(RUMI_TOF_OUT_OF_RANGE, 150, 90) == 0);
    CHECK(r.out.steer == RUMI_STEER_LEFT && r.out.drive_dir == RUMI_DRIVE_FORWARD && r.out.drive_pwm == 115 && r.state() == "TURN");
    // Very close: back up with the wheels turned away from the open side.
    CHECK(r.step(RUMI_TOF_OUT_OF_RANGE, 80, 90) == 0);
    CHECK(r.out.steer == RUMI_STEER_RIGHT && r.out.drive_dir == RUMI_DRIVE_REVERSE && r.out.drive_pwm == 128 && r.state() == "BACK");
    // A sensor without reading (-1): brake.
    CHECK(r.step(-1, 150, 150) == 0);
    CHECK(r.out.drive_dir == RUMI_DRIVE_BRAKE && r.out.drive_pwm == 0 && r.state() == "NO_SENSOR");
    // The same names keep their ids; unknown ids have a name too.
    CHECK(r.step(RUMI_TOF_OUT_OF_RANGE, RUMI_TOF_OUT_OF_RANGE, RUMI_TOF_OUT_OF_RANGE) == 0 && r.state() == "RUN");
    CHECK(std::string(api->state_name(r.inst, 0)) == "DRIVE");
    CHECK(std::string(api->state_name(r.inst, 200)) == "?");
    // The round trip is fast: 200 frames well within the 50 ms period each.
    const auto t0 = Clock::now();
    for (int i = 0; i < 200; ++i) CHECK(r.step(RUMI_TOF_OUT_OF_RANGE, 150, RUMI_TOF_OUT_OF_RANGE) == 0);
    const auto per_frame = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count() / 200;
    CHECK_MSG(per_frame < 10000, std::to_string(per_frame) + " us per frame");
    std::printf("py_drive: %lld us per frame\n", static_cast<long long>(per_frame));
}

// drive.py for the RC_read tests: what RC_read returns for the three sensors,
// sent back as the state name "V<left>_<center>_<right>" (M for a minus sign),
// so the test sees the values after the whole way: core frame -> plug-in ->
// runner.py -> RumiCar.py (RC_read) -> RC_state -> runner.py -> plug-in.
const char* const kReportReads =
    "def report():\n"
    "    v = [RC_read(LEFT), RC_read(CENTER), RC_read(RIGHT)]\n"
    "    RC_state('V' + '_'.join(str(x).replace('-', 'M') for x in v))\n";

void test_read_limit() {
    // RC_read / RC_setReadLimit of RumiCar.py 0.7.0 (the same as RumiCar-lib
    // v0.7.0 on Arduino), with the values the core of ABI 5 sends: 0..2000 mm,
    // -1 (no reading), -3 (out of range).
    {
        // No limit set (the default): the values as the core sends them.
        Run r(make_pack("limit-none", std::string("from RumiCar import *\n") + kReportReads + "def loop():\n    report()\n"));
        CHECK_MSG(r.inst != nullptr, r.error);
        if (!r.inst) return;
        CHECK(r.step(0, 200, 201) == 0 && r.state() == "V0_200_201");
        CHECK(r.step(1999, 2000, 2001) == 0 && r.state() == "V1999_2000_M3");   // above 2000: -3 (the core sends no such value)
        CHECK(r.step(-1, -3, 8190) == 0 && r.state() == "VM1_M3_M3");
    }
    {
        // RC_setReadLimit(200) in setup(): above the limit RC_read returns -3,
        // the limit itself is a distance, -1 and -3 stay.  These are the values
        // a program got from the car up to ABI 4 (cut off at 200 mm by the core).
        Run r(make_pack("limit-200", std::string("from RumiCar import *\n") + kReportReads +
            "def setup():\n"
            "    RC_setup()\n"
            "    if RC_setReadLimit(200) != 1: raise RuntimeError('RC_setReadLimit(200) must return 1')\n"
            "def loop():\n"
            "    report()\n"));
        CHECK_MSG(r.inst != nullptr, r.error);
        if (!r.inst) return;
        CHECK(r.step(200, 201, 2000) == 0 && r.state() == "V200_M3_M3");
        CHECK(r.step(0, 199, -1) == 0 && r.state() == "V0_199_M1");
        CHECK(r.step(-3, 2001, 150) == 0 && r.state() == "VM3_M3_150");
    }
    {
        // One direction only; REAR can be set although this car has no rear sensor (RC_read(REAR) stays -1).
        Run r(make_pack("limit-direction", std::string("from RumiCar import *\n") + kReportReads +
            "def setup():\n"
            "    RC_setup()\n"
            "    if RC_setReadLimit(LEFT, 300) != 1 or RC_setReadLimit(REAR, 100) != 1: raise RuntimeError('rc')\n"
            "    if RC_read(REAR) != -1: raise RuntimeError('RC_read(REAR)')\n"
            "def loop():\n"
            "    report()\n"));
        CHECK_MSG(r.inst != nullptr, r.error);
        if (!r.inst) return;
        CHECK(r.step(300, 301, 301) == 0 && r.state() == "V300_301_301");
        CHECK(r.step(301, 2000, 1500) == 0 && r.state() == "VM3_2000_1500");
    }
    {
        // The values of the limit and the return values, one case per frame:
        // 0 and negative = no limit, above 2000 = 2000, a fraction towards 0, not
        // a number = no limit (with a warning); a bad direction returns 0 and
        // changes nothing; RC_setup() does not reset the limit; a wrong number of
        // arguments is an error (TypeError: that frame brakes).
        Run r(make_pack("limit-values", std::string("from RumiCar import *\n") + kReportReads +
            "frame = 0\n"
            "def need(value, expected):\n"
            "    if value != expected: raise RuntimeError('frame %d: %r, expected %r' % (frame, value, expected))\n"
            "def loop():\n"
            "    global frame\n"
            "    frame += 1\n"
            "    if frame == 1: need(RC_setReadLimit(100), 1)\n"
            "    elif frame == 2: need(RC_setReadLimit(0), 1)\n"
            "    elif frame == 3:\n"
            "        need(RC_setReadLimit(100), 1)\n"
            "        need(RC_setReadLimit(-5), 1)\n"
            "    elif frame == 4: need(RC_setReadLimit(32767), 1)\n"
            "    elif frame == 5: need(RC_setReadLimit(1), 1)\n"
            "    elif frame == 6:\n"
            "        for bad in (4, 5, -1, True, None, 'LEFT'): need(RC_setReadLimit(bad, 50), 0)\n"
            "    elif frame == 7: need(RC_setReadLimit(200.9), 1)\n"
            "    elif frame == 8: need(RC_setReadLimit('abc'), 1)\n"
            "    elif frame == 9: need(RC_setReadLimit(RIGHT, 150), 1)\n"
            "    elif frame == 10: RC_setup()\n"
            "    elif frame == 11: RC_setReadLimit()\n"
            "    elif frame == 12: RC_setReadLimit(LEFT, 100, 1)\n"
            "    report()\n"));
        CHECK_MSG(r.inst != nullptr, r.error);
        if (!r.inst) return;
        struct Case { int L, C, R; const char* state; const char* what; };
        const Case cases[] = {
            {100, 101, 2000, "V100_M3_M3", "limit 100"},
            {100, 101, 2000, "V100_101_2000", "0: no limit"},
            {100, 101, 2000, "V100_101_2000", "negative: no limit"},
            {1999, 2000, 2001, "V1999_2000_M3", "32767: the range of the sensor (2000)"},
            {0, 1, 2, "V0_1_M3", "limit 1"},
            {0, 1, 2, "V0_1_M3", "bad directions: nothing changed"},
            {200, 201, 199, "V200_M3_199", "200.9: 200"},
            {200, 201, 2000, "V200_201_2000", "not a number: no limit"},
            {151, 151, 151, "V151_151_M3", "RIGHT only: 150"},
            {151, 151, 151, "V151_151_M3", "RC_setup() keeps the limit"},
        };
        int n = 0;
        for (const Case& k : cases) {
            ++n;
            CHECK_MSG(r.step(k.L, k.C, k.R) == 0 && r.state() == k.state,
                      "frame " + std::to_string(n) + " (" + k.what + "): " + r.state() + ", expected " + k.state);
        }
        CHECK(r.step(151, 151, 151) == 6);   // RC_setReadLimit(): TypeError, the frame brakes
        CHECK(r.step(151, 151, 151) == 6);   // three arguments: TypeError
        CHECK(r.step(151, 151, 151) == 0 && r.state() == "V151_151_M3");   // the program goes on, the limit as it was
    }
}

void test_sample_limit_gives_the_values_of_the_200_mm_cut_off(const fs::path& sample) {
    // The sample pack with read_limit_mm 200 decides on the car's values of ABI 5
    // (0..2000) exactly as on those of ABI 4 (the core cut them off at 200 mm:
    // -3 above).  Two instances of the pack, frame by frame.
    Run now(sample), before(sample);
    CHECK_MSG(now.inst != nullptr && before.inst != nullptr, now.error + before.error);
    if (!now.inst || !before.inst) return;
    uint32_t x = 2026;
    auto next = [&]() { x = x * 1664525u + 1013904223u; return x >> 8; };
    auto sensor = [&]() {   // what a VL53L0X gives: a distance, 8190 (nothing reflects) or no reading
        const uint32_t kind = next() % 20;
        if (kind == 0) return -1;
        if (kind <= 2) return 8190;
        if (kind <= 11) return static_cast<int>(next() % 260);
        if (kind <= 15) return 150 + static_cast<int>(next() % 101);
        return static_cast<int>(next() % 2601);
    };
    auto core = [](int v, int max_mm) { return v < 0 ? RUMI_TOF_TIMEOUT : v > max_mm ? RUMI_TOF_OUT_OF_RANGE : v; };
    int differ = 0, beyond = 0;
    for (int i = 0; i < 2000; ++i) {
        const int v[3] = {sensor(), sensor(), sensor()};
        const int ra = now.step(core(v[0], RUMI_TOF_MAX_MM), core(v[1], RUMI_TOF_MAX_MM), core(v[2], RUMI_TOF_MAX_MM));
        const int rb = before.step(core(v[0], 200), core(v[1], 200), core(v[2], 200));
        if (ra != rb || now.out.steer != before.out.steer || now.out.steer_pwm != before.out.steer_pwm ||
            now.out.drive_dir != before.out.drive_dir || now.out.drive_pwm != before.out.drive_pwm || now.state() != before.state())
            ++differ;
        for (const int d : v) beyond += (d > 200 && d <= RUMI_TOF_MAX_MM) ? 1 : 0;
    }
    CHECK_MSG(differ == 0, std::to_string(differ) + " frames differ");
    CHECK_MSG(beyond > 500, std::to_string(beyond) + " readings between 200 and 2000 mm");
}

void test_steering_strength() {
    // RC_steer(direc, ipwm=255) of RumiCar.py (from 0.6.0), through the frame protocol
    // (C <seq> <steer> <steer_pwm> <drive_dir> <drive_pwm> <state>) into rumi_command.
    Run r(make_pack("strength",
        "from RumiCar import *\n"
        "def loop():\n"
        "    c = RC_read(CENTER)\n"
        "    RC_drive(FORWARD, 100)\n"
        "    if c == 1: RC_steer(RIGHT, 128)\n"
        "    elif c == 2: RC_steer(LEFT)\n"
        "    elif c == 3: RC_steer(CENTER, 200)\n"
        "    elif c == 4: RC_steer(LEFT, 0)\n"
        "    elif c == 5: RC_steer(RIGHT, 300)\n"
        "    elif c == 6: RC_steer(LEFT, -5)\n"
        "    elif c == 7: RC_steer(RIGHT, 99.9)\n"
        "    elif c == 8:\n"
        "        if RC_steer(RIGHT, 'abc') != 1: raise RuntimeError('a strength that is not a number counts as 0')\n"
        "    elif c == 9:\n"
        "        if RC_steer(True, 100) != 0: raise RuntimeError('True is not a direction')\n"
        "    elif c == 10: RC_steer(LEFT, 254)\n"
        "    elif c == 11: RC_steer(RIGHT, 1)\n"));
    CHECK_MSG(r.inst != nullptr, r.error);
    if (!r.inst) return;
    struct Case { int c; uint8_t steer, steer_pwm; };
    const Case cases[] = {
        {1, RUMI_STEER_RIGHT, 128},
        {2, RUMI_STEER_LEFT, 255},     // one argument: full, as before
        {3, RUMI_STEER_CENTER, 0},     // CENTER ignores the strength
        {4, RUMI_STEER_LEFT, 0},       // 0 = neutral (the core outputs it as CENTER)
        {5, RUMI_STEER_RIGHT, 255},    // clamped
        {6, RUMI_STEER_LEFT, 0},       // clamped
        {7, RUMI_STEER_RIGHT, 99},     // int(): towards 0
        {8, RUMI_STEER_RIGHT, 0},      // not a number: 0
        {9, RUMI_STEER_RIGHT, 0},      // ignored: the previous steering stays
        {10, RUMI_STEER_LEFT, 254},
        {11, RUMI_STEER_RIGHT, 1},
    };
    for (const Case& k : cases) {
        CHECK_MSG(r.step(150, k.c, 150) == 0, "frame " + std::to_string(k.c));
        CHECK_MSG(r.out.steer == k.steer && r.out.steer_pwm == k.steer_pwm,
                  "RC_steer case " + std::to_string(k.c) + ": steer " + std::to_string(r.out.steer) + ", steer_pwm " + std::to_string(r.out.steer_pwm));
        CHECK(r.out.drive_dir == RUMI_DRIVE_FORWARD && r.out.drive_pwm == 100);
    }
}

void test_library_is_the_staged_rumicar_py(const fs::path& logic_dir) {
    // drive.py gets the RumiCar.py next to runner.py (taken by the build from
    // the pinned RumiCar commit), even when the pack carries a file of that
    // name: the runner registers the module before drive.py is loaded.
    const fs::path pack = make_pack("library",
        "import os\n"
        "import RumiCar\n"
        "from RumiCar import *\n"
        "def setup():\n"
        "    if RumiCar.__version__ != '0.7.0': raise RuntimeError('RumiCar.py ' + RumiCar.__version__)\n"
        "    if not os.path.samefile(os.path.dirname(RumiCar.__file__), RC_params()['logic_dir']): raise RuntimeError(RumiCar.__file__)\n"
        "    if type(RumiCar._hw).__name__ != '_Zero2CamHardware': raise RuntimeError('hardware layer ' + type(RumiCar._hw).__name__)\n"
        "def loop():\n"
        "    RC_steer(RIGHT, 77)\n"
        "    RC_drive(REVERSE, 60)\n",
        "{\"logic_dir\": \"" + logic_dir.string() + "\"}");
    write_file(pack / "RumiCar.py", "raise RuntimeError('the pack\\'s own RumiCar.py must not be loaded')\n");
    Run r(pack);
    CHECK_MSG(r.inst != nullptr, r.error);
    if (!r.inst) return;
    CHECK(r.step(100, 100, 100) == 0);
    CHECK(r.out.steer == RUMI_STEER_RIGHT && r.out.steer_pwm == 77 && r.out.drive_dir == RUMI_DRIVE_REVERSE && r.out.drive_pwm == 60);
}

void test_loading_errors() {
    {
        Run r(make_pack("syntax", "from RumiCar import *\ndef loop()\n    RC_drive(FORWARD, 100)\n"));
        CHECK(r.inst == nullptr);
        CHECK_MSG(r.error.find("drive.py の 2 行目") != std::string::npos, r.error);
    }
    {
        Run r(make_pack("noloop", "def setup():\n    pass\n"));
        CHECK(r.inst == nullptr);
        CHECK_MSG(r.error.find("def loop():") != std::string::npos, r.error);
    }
    {
        Run r(make_pack("badparams", "def loop():\n    pass\n", "{\n  \"speed\": 600 600\n}"));
        CHECK(r.inst == nullptr);
        CHECK_MSG(r.error.find("params.json の 2 行目") != std::string::npos, r.error);
    }
    {
        Run r(make_pack("setupfail", "from RumiCar import *\ndef setup():\n    x = RC_params()['missing']\ndef loop():\n    pass\n"));
        CHECK(r.inst == nullptr);
        CHECK_MSG(r.error.find("drive.py の 3 行目") != std::string::npos && r.error.find("KeyError") != std::string::npos, r.error);
    }
    {
        // No drive.py at all.
        fs::create_directories(tmp / "empty");
        Run r(tmp / "empty");
        CHECK(r.inst == nullptr && r.error.find("drive.py") != std::string::npos);
    }
    {
        // Without params.json the program gets {}.
        Run r(make_pack("noparams", "from RumiCar import *\ndef loop():\n    RC_drive(FORWARD, 77 if RC_params() == {} else 0)\n", ""));
        CHECK_MSG(r.inst != nullptr, r.error);
        if (r.inst) CHECK(r.step(100, 100, 100) == 0 && r.out.drive_dir == RUMI_DRIVE_FORWARD && r.out.drive_pwm == 77);
    }
}

void test_runtime_errors() {
    // An exception brakes that frame only; the program keeps running.  Wrong
    // values are not exceptions (as in RumiCar-lib): a duty outside 0..255 is
    // clamped, a bad direction or state name is ignored and returns 0.
    Run r(make_pack("raises",
        "from RumiCar import *\n"
        "def loop():\n"
        "    c = RC_read(CENTER)\n"
        "    print('distance', c)\n"            // print() must not disturb the protocol
        "    if c < 100:\n"
        "        raise RuntimeError('too close')\n"
        "    if c == 111:\n"
        "        RC_drive(FORWARD, 5000)\n"
        "        return\n"
        "    if c == 112:\n"
        "        RC_drive(FORWARD, 200)\n"
        "        if RC_steer(5) != 0: raise RuntimeError('bad steer accepted')\n"
        "        return\n"
        "    if c == 113:\n"
        "        if RC_state('bad name!') != 0: raise RuntimeError('bad state accepted')\n"
        "        if RC_read(7) != -2 or RC_read(REAR) != -1: raise RuntimeError('bad read')\n"
        "        return\n"
        "    if RC_steer(LEFT) != 1 or RC_drive(FORWARD, 150.4) != 1 or RC_state('ok') != 1: raise RuntimeError('rc')\n"));
    CHECK_MSG(r.inst != nullptr, r.error);
    if (!r.inst) return;
    CHECK(r.step(150, 150, 150) == 0 && r.out.drive_dir == RUMI_DRIVE_FORWARD && r.out.drive_pwm == 150 && r.out.steer == RUMI_STEER_LEFT &&
          r.out.steer_pwm == 255 && r.state() == "OK");
    CHECK(r.step(150, 50, 150) == 6);
    CHECK(r.step(150, 111, 150) == 0 && r.out.drive_pwm == 255 && r.out.steer == RUMI_STEER_LEFT);   // clamped; steering kept
    CHECK(r.step(150, 112, 150) == 0 && r.out.drive_pwm == 200 && r.out.steer == RUMI_STEER_LEFT);   // RC_steer(5) ignored
    CHECK(r.step(150, 113, 150) == 0 && r.state() == "OK");                                            // bad name ignored
    CHECK(r.step(150, 150, 150) == 0 && r.out.drive_pwm == 150);
}

void test_hung_program_is_stopped() {
    Run r(make_pack("hangs",
        "import time\n"
        "from RumiCar import *\n"
        "def loop():\n"
        "    if RC_read(CENTER) < 100:\n"
        "        while True:\n"
        "            time.sleep(0.01)\n"
        "    RC_drive(FORWARD, 128)\n"));
    CHECK_MSG(r.inst != nullptr, r.error);
    if (!r.inst) return;
    CHECK(r.step(150, 150, 150) == 0);
    // Every frame returns within the step timeout (30 ms), braking, until 1 s
    // without an answer stops the program for good.
    int late = 0;
    int rc = 0;
    auto worst = std::chrono::microseconds(0);
    const auto t0 = Clock::now();
    for (int i = 0; i < 60 && rc != 5; ++i) {
        const auto s = Clock::now();
        rc = r.step(150, 50, 150);
        worst = std::max(worst, std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - s));
        if (rc == 4) ++late;
    }
    CHECK(rc == 5);
    CHECK_MSG(worst.count() < 45000, std::to_string(worst.count()) + " us at most per frame");
    CHECK(late >= 30);
    CHECK(Clock::now() - t0 < std::chrono::seconds(3));
    CHECK(r.step(150, 150, 150) == 5);   // stays stopped
}

void test_program_that_exits() {
    Run r(make_pack("exits",
        "import os\n"
        "from RumiCar import *\n"
        "def loop():\n"
        "    if RC_read(CENTER) < 100:\n"
        "        os._exit(3)\n"
        "    RC_drive(FORWARD, 200)\n"));
    CHECK_MSG(r.inst != nullptr, r.error);
    if (!r.inst) return;
    CHECK(r.step(150, 150, 150) == 0 && r.out.drive_dir == RUMI_DRIVE_FORWARD && r.out.drive_pwm == 200);
    CHECK(r.step(150, 50, 150) == 5);
    CHECK(r.step(150, 150, 150) == 5);
}

void test_destroy_ends_the_process() {
    const fs::path pack = make_pack("pidfile",
        "import os\n"
        "from RumiCar import *\n"
        "def setup():\n"
        "    open(RC_params()['pidfile'], 'w').write(str(os.getpid()))\n"
        "def loop():\n"
        "    pass\n",
        "{\"pidfile\": \"" + (tmp / "pid.txt").string() + "\"}");
    pid_t pid = 0;
    {
        Run r(pack);
        CHECK_MSG(r.inst != nullptr, r.error);
        std::ifstream in(tmp / "pid.txt");
        in >> pid;
        CHECK(pid > 0 && ::kill(pid, 0) == 0);
    }
    CHECK(pid > 0 && ::kill(pid, 0) != 0);   // gone (and reaped) after destroy
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: test_py_drive <py_drive logic dir> <py-sample pack dir>\n");
        return 2;
    }
    tmp = fs::temp_directory_path() / ("rumicar-py-" + std::to_string(::getpid()));
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    void* lib = ::dlopen((fs::path(argv[1]) / "libpy_drive.so").c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        const char* why = ::dlerror();
        CHECK_MSG(lib != nullptr, why ? why : "dlopen failed");
        return check::summary("test_py_drive");
    }
    auto get = reinterpret_cast<const rumi_logic_api* (*)()>(::dlsym(lib, "rumi_logic_get_api"));
    api = get();
    test_api();
    test_sample_pack(argv[2]);
    test_read_limit();
    test_sample_limit_gives_the_values_of_the_200_mm_cut_off(argv[2]);
    test_steering_strength();
    test_library_is_the_staged_rumicar_py(fs::absolute(argv[1]));
    test_loading_errors();
    test_runtime_errors();
    test_hung_program_is_stopped();
    test_program_that_exits();
    test_destroy_ends_the_process();
    fs::remove_all(tmp);
    return check::summary("test_py_drive");
}
