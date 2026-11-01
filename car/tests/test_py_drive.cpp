// Tests of py_drive, the logic that runs the user's own Python program
// (drive.py), through the real plug-in and runner.py: the sample pack,
// errors while loading (syntax, missing loop, params.json), a program that
// raises, one that hangs, one that exits, wrong values, print(), and that the
// control loop never waits longer than the step timeout.
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
    // Open road (nothing within 20 cm, -3): straight at "speed" (153).
    CHECK(r.step(RUMI_TOF_OUT_OF_RANGE, RUMI_TOF_OUT_OF_RANGE, RUMI_TOF_OUT_OF_RANGE) == 0);
    CHECK(r.out.size == sizeof(rumi_command));
    CHECK(r.out.steer == RUMI_STEER_CENTER && r.out.drive_dir == RUMI_DRIVE_FORWARD && r.out.drive_pwm == 153 && r.out.flags == 0);
    CHECK(r.state() == "RUN");
    // A wall near on the left: steer right.
    CHECK(r.step(40, RUMI_TOF_OUT_OF_RANGE, RUMI_TOF_OUT_OF_RANGE) == 0);
    CHECK(r.out.steer == RUMI_STEER_RIGHT && r.state() == "RUN");
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
    CHECK(r.step(150, 150, 150) == 0 && r.out.drive_dir == RUMI_DRIVE_FORWARD && r.out.drive_pwm == 150 && r.out.steer == RUMI_STEER_LEFT && r.state() == "OK");
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
    test_loading_errors();
    test_runtime_errors();
    test_hung_program_is_stopped();
    test_program_that_exits();
    test_destroy_ends_the_process();
    fs::remove_all(tmp);
    return check::summary("test_py_drive");
}
