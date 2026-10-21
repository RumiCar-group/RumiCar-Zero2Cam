// RumiCar-Zero2Cam — C ABI entry point of the ToF driving logic plug-in.
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

#include "params_json.hpp"
#include "rumicar/logic_api.h"
#include "tof_logic.hpp"

#ifndef TOF_DRIVE_VERSION
#define TOF_DRIVE_VERSION "0.0.0"
#endif

namespace {

using rumicar::tof::Params;
using rumicar::tof::TofDriveLogic;

struct Instance {
    Params params;
    TofDriveLogic logic;
    explicit Instance(const Params& p) : params(p), logic(p) {}
};

void put_err(char* err, size_t err_len, const std::string& msg) {
    if (!err || err_len == 0) return;
    std::snprintf(err, err_len, "%s", msg.c_str());
}

void* tof_create(const char* pack_dir, char* err, size_t err_len) {
    try {
        if (!pack_dir || !*pack_dir) throw std::runtime_error("pack directory not given");
        const std::string path = std::string(pack_dir) + "/params.json";
        std::ifstream in(path);
        if (!in) throw std::runtime_error("cannot open " + path);
        std::stringstream ss;
        ss << in.rdbuf();
        const Params p = rumicar::tof::parse_params_json(ss.str());
        return new Instance(p);
    } catch (const std::exception& e) {
        put_err(err, err_len, e.what());
        return nullptr;
    }
}

void tof_destroy(void* inst) { delete static_cast<Instance*>(inst); }

int tof_self_test(void* inst, char* err, size_t err_len) {
    auto* in = static_cast<Instance*>(inst);
    if (!in) { put_err(err, err_len, "null instance"); return 1; }
    const std::string v = rumicar::tof::validate(in->params);
    if (!v.empty()) { put_err(err, err_len, v); return 2; }

    // Exercise the logic on a synthetic open-corridor sequence and check that
    // the drive command rises monotonically and never exceeds v_max.
    TofDriveLogic probe(in->params);
    int last = 0;
    for (uint32_t i = 0; i < 50; ++i) {
        rumi_sensor_frame f{};
        f.size = sizeof(f);
        f.seq = i;
        f.t_us = 1'000'000ull + static_cast<uint64_t>(i) * 50'000ull;
        f.tof_mm[RUMI_TOF_LEFT] = RUMI_TOF_NO_TARGET;
        f.tof_mm[RUMI_TOF_CENTER] = RUMI_TOF_NO_TARGET;
        f.tof_mm[RUMI_TOF_RIGHT] = RUMI_TOF_NO_TARGET;
        rumi_command c{};
        if (!probe.step(f, c)) { put_err(err, err_len, "step rejected a well-formed frame"); return 3; }
        if (c.drive_permille < last || c.drive_permille > in->params.v_max) {
            put_err(err, err_len, "drive ramp is not monotonic within v_max");
            return 4;
        }
        last = c.drive_permille;
    }
    if (last != in->params.v_max) { put_err(err, err_len, "drive did not reach v_max in 2.5 s"); return 5; }
    return 0;
}

int tof_step(void* inst, const rumi_sensor_frame* frame, rumi_command* out) {
    auto* in = static_cast<Instance*>(inst);
    if (!in || !frame || !out) return 1;
    return in->logic.step(*frame, *out) ? 0 : 2;
}

const char* tof_state_name(void*, uint8_t state) {
    return rumicar::tof::state_name(static_cast<rumicar::tof::State>(state));
}

// No auxiliary output: this logic only drives on the distance sensors.
const char* tof_aux_name(void*, uint8_t) { return nullptr; }

const rumi_logic_api kApi = {
    RUMI_LOGIC_ABI_VERSION,
    "tof_drive",
    TOF_DRIVE_VERSION,
    &tof_create,
    &tof_destroy,
    &tof_self_test,
    &tof_step,
    &tof_state_name,
    &tof_aux_name,
};

} // namespace

extern "C" __attribute__((visibility("default"))) const rumi_logic_api* rumi_logic_get_api(void) {
    return &kApi;
}
