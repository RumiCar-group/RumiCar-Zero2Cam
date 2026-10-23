// RumiCar-Zero2Cam — C ABI entry point of py_drive: the driving logic that
// runs the user's own Python program (drive.py in the pack, docs 3.12).
#include <chrono>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <memory>
#include <string>

#include <dlfcn.h>

#include "py_process.hpp"
#include "rumicar/logic_api.h"

#ifndef PY_DRIVE_VERSION
#define PY_DRIVE_VERSION "0.0.0"
#endif

namespace {

using namespace std::chrono_literals;
using rumicar::py::PyProcess;
using rumicar::py::Reply;

constexpr const char* kPython = "/usr/bin/python3";
constexpr auto kStartTimeout = 15s;        // loading python and drive.py on a Zero 2 W
constexpr auto kStepTimeout = 30ms;        // per frame (the loop runs every 50 ms)
constexpr auto kSilentLimit = 1000ms;      // no answer for this long: the program is stopped
constexpr size_t kMaxStates = 64;          // distinct state names a program may use

// State id 0 is "DRIVE" (the program set none); the program's own names follow.
// RUMI state ids are what the core logs and the PC shows by name.
struct Instance {
    std::unique_ptr<PyProcess> proc;
    std::deque<std::string> states{"DRIVE"};   // deque: c_str() stays valid as names are added
    std::chrono::steady_clock::time_point last_answer = std::chrono::steady_clock::now();
    uint64_t t0_us = 0;
    bool have_t0 = false;
    bool stopped_reported = false;

    uint8_t state_id(const std::string& name) {
        if (name.empty() || name == "-") return 0;
        for (size_t i = 0; i < states.size(); ++i) {
            if (states[i] == name) return static_cast<uint8_t>(i);
        }
        if (states.size() >= kMaxStates) return 0;
        states.push_back(name);
        return static_cast<uint8_t>(states.size() - 1);
    }
};

void put_err(char* err, size_t err_len, const std::string& msg) {
    if (!err || err_len == 0) return;
    std::snprintf(err, err_len, "%s", msg.c_str());
}

// runner.py sits next to this library in the logic directory.
std::string runner_path() {
    Dl_info info{};
    if (::dladdr(reinterpret_cast<void*>(&put_err), &info) == 0 || !info.dli_fname) return "runner.py";
    return (std::filesystem::path(info.dli_fname).parent_path() / "runner.py").string();
}

void* py_create(const char* pack_dir, char* err, size_t err_len) {
    try {
        if (!pack_dir || !*pack_dir) throw std::runtime_error("pack directory not given");
        if (!std::filesystem::is_regular_file(std::filesystem::path(pack_dir) / "drive.py"))
            throw std::runtime_error(std::string("py_drive: ") + pack_dir + " has no drive.py");
        auto in = std::make_unique<Instance>();
        in->proc = std::make_unique<PyProcess>(kPython, runner_path(), pack_dir, kStartTimeout);
        return in.release();
    } catch (const std::exception& e) {
        put_err(err, err_len, e.what());
        return nullptr;
    }
}

void py_destroy(void* inst) { delete static_cast<Instance*>(inst); }

int py_self_test(void* inst, char* err, size_t err_len) {
    auto* in = static_cast<Instance*>(inst);
    if (!in || !in->proc) { put_err(err, err_len, "null instance"); return 1; }
    if (!in->proc->ping(2s)) {
        put_err(err, err_len, "py_drive: drive.py does not answer (see the core's log)");
        return 2;
    }
    return 0;
}

int py_step(void* inst, const rumi_sensor_frame* frame, rumi_command* out) {
    auto* in = static_cast<Instance*>(inst);
    if (!in || !frame || !out) return 1;
    if (frame->size < sizeof(rumi_sensor_frame)) return 2;
    *out = rumi_command{};
    out->size = sizeof(rumi_command);
    if (!in->have_t0) {
        in->t0_us = frame->t_us;
        in->have_t0 = true;
    }
    if (!in->proc->alive()) return 5;   // stopped: brake until the next activation

    Reply r;
    const double t_ms = static_cast<double>(frame->t_us - in->t0_us) / 1000.0;
    const auto now = std::chrono::steady_clock::now();
    if (!in->proc->frame(frame->seq, t_ms, frame->tof_mm, kStepTimeout, r)) {
        if (!in->proc->alive()) {
            if (!in->stopped_reported) std::fprintf(stderr, "py_drive: drive.py has ended; the car brakes until the program is activated again\n");
            in->stopped_reported = true;
            return 5;
        }
        if (now - in->last_answer >= kSilentLimit) {
            std::fprintf(stderr, "py_drive: drive.py did not answer for %lld ms (an endless loop or a long wait in loop()?); it was stopped\n",
                         static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(now - in->last_answer).count()));
            in->proc->stop();
            in->stopped_reported = true;
            return 5;
        }
        return 4;   // late this frame: brake for it
    }
    in->last_answer = std::chrono::steady_clock::now();
    if (r.kind == Reply::Kind::Error) return 6;   // drive.py raised; the runner printed the traceback

    out->steer = r.steer;
    out->drive_permille = r.drive;
    out->flags = r.brake ? RUMI_CMD_FLAG_BRAKE : 0u;
    out->state = in->state_id(r.state);
    return 0;
}

const char* py_state_name(void* inst, uint8_t state) {
    auto* in = static_cast<Instance*>(inst);
    if (!in || state >= in->states.size()) return "?";
    return in->states[state].c_str();
}

const char* py_aux_name(void*, uint8_t) { return nullptr; }

const rumi_logic_api kApi = {
    RUMI_LOGIC_ABI_VERSION,
    "py_drive",
    PY_DRIVE_VERSION,
    &py_create,
    &py_destroy,
    &py_self_test,
    &py_step,
    &py_state_name,
    &py_aux_name,
};

} // namespace

extern "C" __attribute__((visibility("default"))) const rumi_logic_api* rumi_logic_get_api(void) {
    return &kApi;
}
