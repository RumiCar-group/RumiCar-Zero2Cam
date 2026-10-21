// Golden tests of the cam_drive vision layer against car/tools/rumicar_vision.py:
// the same frame must give exactly the same features, and the same pack the
// same detector outputs (data in car/tests/data/vision, made by make_golden.py).
// Usage: test_vision <data dir>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "check.hpp"
#include "vision_eval.hpp"
#include "vision_features.hpp"
#include "vision_pack.hpp"

using namespace rumicar::vision;
using json = nlohmann::json;

namespace {

struct Ppm {
    unsigned w = 0, h = 0;
    std::vector<uint8_t> rgb;
};

Ppm read_ppm(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path);
    std::string magic;
    unsigned maxval = 0;
    Ppm p;
    in >> magic >> p.w >> p.h >> maxval;
    if (magic != "P6" || maxval != 255) throw std::runtime_error("not a P6 ppm: " + path);
    in.get();   // single whitespace after maxval
    p.rgb.resize(static_cast<size_t>(p.w) * p.h * 3);
    in.read(reinterpret_cast<char*>(p.rgb.data()), static_cast<std::streamsize>(p.rgb.size()));
    if (!in) throw std::runtime_error("short ppm: " + path);
    return p;
}

std::vector<uint8_t> read_bytes(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool throws(const std::function<void()>& f, std::string* msg = nullptr) {
    try { f(); } catch (const std::exception& e) { if (msg) *msg = e.what(); return true; }
    return false;
}

FeatureConfig config_from(const json& j) {
    FeatureConfig c;
    c.downscale = j["downscale"].get<unsigned>();
    c.grid_x = j["grid_x"].get<unsigned>();
    c.grid_y = j["grid_y"].get<unsigned>();
    c.s_min = j["s_min"].get<uint8_t>();
    c.v_min = j["v_min"].get<uint8_t>();
    c.v_white = j["v_white"].get<uint8_t>();
    return c;
}

void test_pixels(const json& golden, const FeatureConfig& cfg) {
    for (const auto& px : golden["pixels"]) {
        const auto r = px["rgb"][0].get<uint8_t>(), g = px["rgb"][1].get<uint8_t>(), b = px["rgb"][2].get<uint8_t>();
        const int hue = px["hue"].get<int>();
        if (hue >= 0) CHECK_MSG(hue_degrees(r, g, b) == static_cast<unsigned>(hue), "hue of " + px["rgb"].dump());
        CHECK_MSG(static_cast<int>(classify(r, g, b, cfg)) == px["class"].get<int>(), "class of " + px["rgb"].dump());
    }
    // Hue bins are centred on the primaries: 330..30 is red, 30..90 yellow, ...
    CHECK(classify(255, 0, 0, cfg) == Red && classify(255, 0, 60, cfg) == Red && classify(255, 200, 0, cfg) == Yellow);
    CHECK(classify(0, 255, 0, cfg) == Green && classify(0, 255, 255, cfg) == Cyan && classify(0, 0, 255, cfg) == Blue);
    CHECK(classify(255, 0, 255, cfg) == Magenta);
    CHECK(classify(200, 200, 200, cfg) == Bright && classify(20, 20, 20, cfg) == Dark);
    CHECK(classify(255, 230, 230, cfg) == Bright);   // too little saturation for a hue
    CHECK(classify(40, 0, 0, cfg) == Dark);           // too dark for a hue
}

void test_features_match_python(const json& golden, const Ppm& frame, const FeatureConfig& cfg) {
    CHECK(validate_config(cfg, frame.w, frame.h).empty());
    const SmallImage small = downscale(frame.rgb.data(), frame.w, frame.h, frame.w * 3, cfg.downscale);
    CHECK(small.width == golden["small"][0].get<unsigned>() && small.height == golden["small"][1].get<unsigned>());
    std::vector<int32_t> f;
    unsigned cw = 0, ch = 0;
    compute_features(small, cfg, f, &cw, &ch);
    CHECK(cw == 8 && ch == 8);
    const auto& expected = golden["features"];
    CHECK(f.size() == expected.size() && f.size() == golden["dims"].get<size_t>());
    size_t mismatches = 0;
    for (size_t k = 0; k < f.size() && k < expected.size(); ++k) {
        if (f[k] != expected[k].get<int32_t>()) {
            if (mismatches < 5) std::fprintf(stderr, "feature %zu: C++ %d, Python %d\n", k, f[k], expected[k].get<int32_t>());
            ++mismatches;
        }
    }
    CHECK_MSG(mismatches == 0, std::to_string(mismatches) + " features differ from the Python implementation");
    // The convenience wrapper gives the same vector, and a strided image too.
    std::vector<int32_t> f2;
    extract_features(frame.rgb.data(), frame.w, frame.h, frame.w * 3, cfg, f2);
    CHECK(f2 == f);
    std::vector<uint8_t> padded(static_cast<size_t>(frame.w * 3 + 16) * frame.h, 0);
    for (unsigned y = 0; y < frame.h; ++y)
        std::copy(frame.rgb.begin() + static_cast<long>(y) * frame.w * 3, frame.rgb.begin() + static_cast<long>(y + 1) * frame.w * 3,
                  padded.begin() + static_cast<long>(y) * (frame.w * 3 + 16));
    extract_features(padded.data(), frame.w, frame.h, frame.w * 3 + 16, cfg, f2);
    CHECK(f2 == f);
    // Every cell's class counts add up to the cell size.
    for (unsigned c = 0; c < cfg.grid_x * cfg.grid_y; ++c) {
        int32_t sum = 0;
        for (unsigned k = 0; k < kClassCount; ++k) sum += f[c * kFeaturesPerCell + k];
        CHECK(sum == static_cast<int32_t>(cw * ch));
    }
}

void test_pack_matches_python(const json& golden, const std::vector<uint8_t>& bytes, const Ppm& frame) {
    CHECK(bytes.size() == golden["pack_bytes"].get<size_t>());
    const VisionPack p = parse_vision_pack(bytes);
    CHECK(p.config.downscale == 5 && p.config.grid_x == 8 && p.config.grid_y == 6);
    CHECK(p.image_w == frame.w && p.image_h == frame.h);
    CHECK(p.detectors.size() == 3);
    for (unsigned k = 0; k < kFeaturesPerCell; ++k) {
        CHECK(std::fabs(p.mean[k] - golden["mean"][k].get<float>()) < 1e-6f);
        CHECK(std::fabs(p.std[k] - golden["std"][k].get<float>()) < 1e-6f);
    }
    std::vector<int32_t> f;
    extract_features(frame.rgb.data(), frame.w, frame.h, frame.w * 3, p.config, f);
    for (size_t i = 0; i < p.detectors.size(); ++i) {
        const auto& g = golden["detectors"][i];
        CHECK(p.detectors[i].name == g["name"].get<std::string>());
        const DetectorOutput o = evaluate_detector(p, p.detectors[i], f);
        CHECK_MSG(std::fabs(o.score - g["score"].get<float>()) < 1e-4f, p.detectors[i].name + " score " + std::to_string(o.score));
        CHECK(o.cx == g["cx"].get<unsigned>() && o.cy == g["cy"].get<unsigned>());
        CHECK(o.cells == g["cells"].get<unsigned>());
        CHECK(o.present == g["present"].get<bool>());
    }
    // The self-test block made from the same frame agrees with a fresh computation.
    CHECK(p.selftest.present);
    CHECK(p.selftest.features == f);
    std::vector<int32_t> from_small;
    compute_features(p.selftest.image, p.config, from_small);
    CHECK(from_small == f);
    for (size_t i = 0; i < p.detectors.size(); ++i) {
        CHECK(std::fabs(p.selftest.scores[i] - evaluate_detector(p, p.detectors[i], from_small).score) < 1e-4f);
    }
    // Byte-identical round trip through the C++ encoder.
    CHECK(encode_vision_pack(p) == bytes);
    CHECK(crc32(bytes.data(), bytes.size() - 4) == golden["pack_crc32"].get<uint32_t>());
}

void test_pack_rejections(const std::vector<uint8_t>& bytes) {
    std::string msg;
    std::vector<uint8_t> bad = bytes;
    bad[200] ^= 0x01;
    CHECK(throws([&] { parse_vision_pack(bad); }, &msg) && msg.find("CRC") != std::string::npos);
    bad = bytes;
    bad.push_back(0);
    CHECK(throws([&] { parse_vision_pack(bad); }));
    CHECK(throws([&] { parse_vision_pack(std::vector<uint8_t>(bytes.begin(), bytes.begin() + 100)); }));

    VisionPack p = parse_vision_pack(bytes);
    auto rejects = [&](const std::function<void(VisionPack&)>& mutate, const char* what) {
        VisionPack q = p;
        mutate(q);
        const auto enc = encode_vision_pack(q);   // re-signed, so only the content is at fault
        CHECK_MSG(throws([&] { parse_vision_pack(enc); }), what);
    };
    rejects([](VisionPack& q) { q.std[3] = 0.0f; }, "zero std");
    rejects([](VisionPack& q) { q.detectors[0].name = "Red Light"; }, "invalid name");
    rejects([](VisionPack& q) { q.detectors[1].name = q.detectors[0].name; }, "duplicate name");
    rejects([](VisionPack& q) { q.detectors[0].weights.resize(50); }, "wrong dims");
    rejects([](VisionPack& q) { q.detectors[0].thr_off = 0.9f; }, "thr_off above thr_on");
    rejects([](VisionPack& q) { q.detectors[0].confirm_frames = 0; }, "confirm 0");
    rejects([](VisionPack& q) { q.detectors[0].row_min = 7; }, "row_min beyond the grid");
    rejects([](VisionPack& q) { q.detectors[0].min_cells = 0; }, "min_cells 0");
    rejects([](VisionPack& q) { q.config.grid_x = 40; }, "cells too small");
    rejects([](VisionPack& q) { q.selftest.image.width = 32; }, "self-test size");
    // Without a self-test block the pack is still valid.
    VisionPack q = p;
    q.selftest = SelfTest{};
    CHECK(!throws([&] { parse_vision_pack(encode_vision_pack(q)); }));
    CHECK(valid_detector_name("traffic_light_red") && !valid_detector_name("") && !valid_detector_name("a-b") &&
          !valid_detector_name(std::string(33, 'a')));
}

void test_temporal_confirmation(const std::vector<uint8_t>& bytes, const Ppm& frame) {
    VisionPack p = parse_vision_pack(bytes);
    p.selftest = SelfTest{};
    // One Global detector on the "bright" counts with confirm 3 / release 2.
    p.detectors.resize(1);
    Detector& d = p.detectors[0];
    d.kind = DetectorKind::Global;
    d.name = "bright";
    d.weights.assign(p.config.dims(), 0.0f);
    for (unsigned c = 0; c < p.config.grid_x * p.config.grid_y; ++c) d.weights[c * kFeaturesPerCell + Bright] = 0.5f;
    d.bias = -1.0f;
    d.thr_on = 0.6f;
    d.thr_off = 0.4f;
    d.confirm_frames = 3;
    d.release_frames = 2;
    VisionEvaluator ev(p);
    std::vector<int32_t> dark(p.config.dims(), 0), bright(p.config.dims(), 0);
    for (unsigned c = 0; c < p.config.grid_x * p.config.grid_y; ++c) {
        dark[c * kFeaturesPerCell + Dark] = 64;
        bright[c * kFeaturesPerCell + Bright] = 64;
    }
    const float s_bright = evaluate_detector(p, d, bright).score, s_dark = evaluate_detector(p, d, dark).score;
    CHECK(s_bright > 0.6f && s_dark < 0.4f);
    CHECK(!ev.step(bright)[0].active);   // 1
    CHECK(!ev.step(bright)[0].active);   // 2
    CHECK(ev.step(bright)[0].active);    // 3: confirmed
    CHECK(ev.step(dark)[0].active);      // 1 below: still active
    CHECK(!ev.step(dark)[0].active);     // 2 below: released
    CHECK(!ev.step(bright)[0].active);
    CHECK(!ev.step(dark)[0].active);     // an interruption restarts the count
    CHECK(!ev.step(bright)[0].active);
    CHECK(!ev.step(bright)[0].active);
    CHECK(ev.step(bright)[0].active);
    ev.reset();
    CHECK(!ev.step(bright)[0].active);
    CHECK(ev.find("bright") == 0 && ev.find("none") == -1);
    // Local detectors report the best cell and the count within their rows.
    const VisionPack full = parse_vision_pack(bytes);
    std::vector<int32_t> f;
    extract_features(frame.rgb.data(), frame.w, frame.h, frame.w * 3, full.config, f);
    Detector rows = full.detectors[0];
    rows.row_min = 5;
    rows.row_max = 5;
    const DetectorOutput o = evaluate_detector(full, rows, f);
    CHECK(o.cy == 5);
    CHECK(o.cells <= full.config.grid_x);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: test_vision <data dir>\n");
        return 2;
    }
    const std::string dir = argv[1];
    json golden;
    {
        std::ifstream in(dir + "/features.json");
        in >> golden;
    }
    const Ppm frame = read_ppm(dir + "/frame.ppm");
    const std::vector<uint8_t> pack = read_bytes(dir + "/pack.bin");
    const FeatureConfig cfg = config_from(golden["config"]);
    test_pixels(golden, cfg);
    test_features_match_python(golden, frame, cfg);
    test_pack_matches_python(golden, pack, frame);
    test_pack_rejections(pack);
    test_temporal_confirmation(pack, frame);
    return check::summary("test_vision");
}
