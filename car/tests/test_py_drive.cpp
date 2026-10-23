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
    // Open road: straight at "speed" (600).
    CHECK(r.step(RUMI_TOF_NO_TARGET, RUMI_TOF_NO_TARGET, RUMI_TOF_NO_TARGET) == 0);
    CHECK(r.out.size == sizeof(rumi_command));
    CHECK(r.out.steer == RUMI_STEER_CENTER && r.out.drive_permille == 600 && r.out.flags == 0);
    CHECK(r.state() == "RUN");
    // A wall near on the left: steer right.
    CHECK(r.step(40, RUMI_TOF_NO_TARGET, 300) == 0);
    CHECK(r.out.steer == RUMI_STEER_RIGHT && r.state() == "RUN");
    // Something ahead, more room on the left: turn left slowly.
    CHECK(r.step(240, 150, 90) == 0);
    CHECK(r.out.steer == RUMI_STEER_LEFT && r.out.drive_permille == 450 && r.state() == "TURN");
    // Very close: back up with the wheels turned away from the open side.
    CHECK(r.step(240, 80, 90) == 0);
    CHECK(r.out.steer == RUMI_STEER_RIGHT && r.out.drive_permille == -500 && r.state() == "BACK");
    // A sensor without reading (-1): brake.
    CHECK(r.step(-1, 300, 300) == 0);
    CHECK((r.out.flags & RUMI_CMD_FLAG_BRAKE) != 0 && r.out.drive_permille == 0 && r.state() == "NO_SENSOR");
    // The same names keep their ids; unknown ids have a name too.
    CHECK(r.step(RUMI_TOF_NO_TARGET, RUMI_TOF_NO_TARGET, RUMI_TOF_NO_TARGET) == 0 && r.state() == "RUN");
    CHECK(std::string(api->state_name(r.inst, 0)) == "DRIVE");
    CHECK(std::string(api->state_name(r.inst, 200)) == "?");
    // The round trip is fast: 200 frames well within the 50 ms period each.
    const auto t0 = Clock::now();
    for (int i = 0; i < 200; ++i) CHECK(r.step(RUMI_TOF_NO_TARGET, 500, RUMI_TOF_NO_TARGET) == 0);
    const auto per_frame = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count() / 200;
    CHECK_MSG(per_frame < 10000, std::to_string(per_frame) + " us per frame");
    std::printf("py_drive: %lld us per frame\n", static_cast<long long>(per_frame));
}

void test_loading_errors() {
    {
        Run r(make_pack("syntax", "def loop(car)\n    car.drive(500)\n"));
        CHECK(r.inst == nullptr);
        CHECK_MSG(r.error.find("drive.py の 1 行目") != std::string::npos, r.error);
    }
    {
        Run r(make_pack("noloop", "def setup(car):\n    pass\n"));
        CHECK(r.inst == nullptr);
        CHECK_MSG(r.error.find("def loop(car):") != std::string::npos, r.error);
    }
    {
        Run r(make_pack("badparams", "def loop(car):\n    pass\n", "{\n  \"speed\": 600 600\n}"));
        CHECK(r.inst == nullptr);
        CHECK_MSG(r.error.find("params.json の 2 行目") != std::string::npos, r.error);
    }
    {
        Run r(make_pack("setupfail", "def setup(car):\n    x = car.params['missing']\ndef loop(car):\n    pass\n"));
        CHECK(r.inst == nullptr);
        CHECK_MSG(r.error.find("drive.py の 2 行目") != std::string::npos && r.error.find("KeyError") != std::string::npos, r.error);
    }
    {
        // No drive.py at all.
        fs::create_directories(tmp / "empty");
        Run r(tmp / "empty");
        CHECK(r.inst == nullptr && r.error.find("drive.py") != std::string::npos);
    }
    {
        // Without params.json the program gets {}.
        Run r(make_pack("noparams", "def loop(car):\n    car.drive(300 if car.params == {} else 0)\n", ""));
        CHECK_MSG(r.inst != nullptr, r.error);
        if (r.inst) CHECK(r.step(100, 100, 100) == 0 && r.out.drive_permille == 300);
    }
}

void test_runtime_errors() {
    // An exception brakes that frame only; the program keeps running.  Wrong values are errors too.
    Run r(make_pack("raises",
        "def loop(car):\n"
        "    print('distance', car.center)\n"            // print() must not disturb the protocol
        "    if car.center < 100:\n"
        "        raise RuntimeError('too close')\n"
        "    if car.center == 111:\n"
        "        car.drive(5000)\n"
        "    if car.center == 112:\n"
        "        car.steer(2)\n"
        "    if car.center == 113:\n"
        "        car.state('bad name!')\n"
        "    car.steer('left')\n"
        "    car.drive(400.4)\n"
        "    car.state('ok')\n"));
    CHECK_MSG(r.inst != nullptr, r.error);
    if (!r.inst) return;
    CHECK(r.step(500, 500, 500) == 0 && r.out.drive_permille == 400 && r.out.steer == RUMI_STEER_LEFT && r.state() == "OK");
    CHECK(r.step(500, 50, 500) == 6);
    CHECK(r.step(500, 111, 500) == 6);
    CHECK(r.step(500, 112, 500) == 6);
    CHECK(r.step(500, 113, 500) == 6);
    CHECK(r.step(500, 500, 500) == 0 && r.out.drive_permille == 400);
}

void test_hung_program_is_stopped() {
    Run r(make_pack("hangs",
        "import time\n"
        "def loop(car):\n"
        "    if car.center < 100:\n"
        "        while True:\n"
        "            time.sleep(0.01)\n"
        "    car.drive(500)\n"));
    CHECK_MSG(r.inst != nullptr, r.error);
    if (!r.inst) return;
    CHECK(r.step(500, 500, 500) == 0);
    // Every frame returns within the step timeout (30 ms), braking, until 1 s
    // without an answer stops the program for good.
    int late = 0;
    int rc = 0;
    auto worst = std::chrono::microseconds(0);
    const auto t0 = Clock::now();
    for (int i = 0; i < 60 && rc != 5; ++i) {
        const auto s = Clock::now();
        rc = r.step(500, 50, 500);
        worst = std::max(worst, std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - s));
        if (rc == 4) ++late;
    }
    CHECK(rc == 5);
    CHECK_MSG(worst.count() < 45000, std::to_string(worst.count()) + " us at most per frame");
    CHECK(late >= 30);
    CHECK(Clock::now() - t0 < std::chrono::seconds(3));
    CHECK(r.step(500, 500, 500) == 5);   // stays stopped
}

void test_program_that_exits() {
    Run r(make_pack("exits",
        "import os\n"
        "def loop(car):\n"
        "    if car.center < 100:\n"
        "        os._exit(3)\n"
        "    car.drive(200)\n"));
    CHECK_MSG(r.inst != nullptr, r.error);
    if (!r.inst) return;
    CHECK(r.step(500, 500, 500) == 0 && r.out.drive_permille == 200);
    CHECK(r.step(500, 50, 500) == 5);
    CHECK(r.step(500, 500, 500) == 5);
}

void test_destroy_ends_the_process() {
    const fs::path pack = make_pack("pidfile",
        "import os\n"
        "def setup(car):\n"
        "    open(car.params['pidfile'], 'w').write(str(os.getpid()))\n"
        "def loop(car):\n"
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
