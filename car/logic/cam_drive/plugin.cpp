// RumiCar-Zero2Cam — C ABI entry point of the camera driving logic plug-in (cam_drive).
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>

#include "cam_logic.hpp"
#include "cam_params.hpp"
#include "rumicar/logic_api.h"
#include "vision_features.hpp"
#include "vision_pack.hpp"

#ifndef CAM_DRIVE_VERSION
#define CAM_DRIVE_VERSION "0.0.0"
#endif

namespace {

using rumicar::cam::CamLogic;
using rumicar::cam::CamParams;

struct Instance {
    CamParams params;
    std::optional<rumicar::vision::VisionPack> pack;   // kept for self_test (the logic holds its own copy)
    CamLogic logic;
    Instance(const CamParams& p, std::optional<rumicar::vision::VisionPack> v) : params(p), pack(v), logic(p, std::move(v)) {}
};

void put_err(char* err, size_t err_len, const std::string& msg) {
    if (!err || err_len == 0) return;
    std::snprintf(err, err_len, "%s", msg.c_str());
}

std::string read_text(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open " + path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void* cam_create(const char* pack_dir, char* err, size_t err_len) {
    try {
        if (!pack_dir || !*pack_dir) throw std::runtime_error("pack directory not given");
        const std::string dir(pack_dir);
        const CamParams params = rumicar::cam::parse_cam_params_json(read_text(dir + "/params.json"));
        std::optional<rumicar::vision::VisionPack> pack;
        const std::string vision_path = dir + "/vision.bin";
        if (std::filesystem::exists(vision_path)) pack = rumicar::vision::load_vision_pack(vision_path);
        return new Instance(params, std::move(pack));
    } catch (const std::exception& e) {
        put_err(err, err_len, e.what());
        return nullptr;
    }
}

void cam_destroy(void* inst) { delete static_cast<Instance*>(inst); }

int cam_self_test(void* inst, char* err, size_t err_len) {
    auto* in = static_cast<Instance*>(inst);
    if (!in) { put_err(err, err_len, "null instance"); return 1; }
    // The ToF part: an open corridor must ramp up to v_max within 2.5 s.
    {
        rumicar::tof::TofDriveLogic probe(in->params.tof);
        int last = 0;
        for (uint32_t i = 0; i < 50; ++i) {
            rumi_sensor_frame f{};
            f.size = sizeof(f);
            f.seq = i;
            f.t_us = 1'000'000ull + static_cast<uint64_t>(i) * 50'000ull;
            f.tof_mm[0] = f.tof_mm[1] = f.tof_mm[2] = RUMI_TOF_NO_TARGET;
            rumi_command c{};
            if (!probe.step(f, c)) { put_err(err, err_len, "tof: step rejected a well-formed frame"); return 3; }
            if (c.drive_permille < last || c.drive_permille > in->params.tof.v_max) { put_err(err, err_len, "tof: drive ramp is not monotonic within v_max"); return 4; }
            last = c.drive_permille;
        }
        if (last != in->params.tof.v_max) { put_err(err, err_len, "tof: drive did not reach v_max in 2.5 s"); return 5; }
    }
    // The vision part: the pack's own self-test block must reproduce on this car.
    if (in->pack && in->pack->selftest.present) {
        const auto& p = *in->pack;
        std::vector<int32_t> f;
        rumicar::vision::compute_features(p.selftest.image, p.config, f);
        if (f != p.selftest.features) { put_err(err, err_len, "vision: features of the self-test image differ from the pack's expectation"); return 6; }
        for (size_t i = 0; i < p.detectors.size(); ++i) {
            const float s = rumicar::vision::evaluate_detector(p, p.detectors[i], f).score;
            if (std::fabs(s - p.selftest.scores[i]) > 1e-3f) {
                put_err(err, err_len, "vision: detector '" + p.detectors[i].name + "' scores " + std::to_string(s) + " on the self-test image, expected " +
                                      std::to_string(p.selftest.scores[i]));
                return 7;
            }
        }
    }
    return 0;
}

int cam_step(void* inst, const rumi_sensor_frame* frame, rumi_command* out) {
    auto* in = static_cast<Instance*>(inst);
    if (!in || !frame || !out) return 1;
    return in->logic.step(*frame, *out);
}

const char* cam_state_name(void*, uint8_t state) { return rumicar::cam::state_name(state); }

const char* cam_aux_name(void* inst, uint8_t index) {
    auto* in = static_cast<Instance*>(inst);
    return in ? in->logic.aux_name(index) : nullptr;
}

const rumi_logic_api kApi = {
    RUMI_LOGIC_ABI_VERSION,
    "cam_drive",
    CAM_DRIVE_VERSION,
    &cam_create,
    &cam_destroy,
    &cam_self_test,
    &cam_step,
    &cam_state_name,
    &cam_aux_name,
};

} // namespace

extern "C" __attribute__((visibility("default"))) const rumi_logic_api* rumi_logic_get_api(void) {
    return &kApi;
}
