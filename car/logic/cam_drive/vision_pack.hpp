// RumiCar-Zero2Cam — the learned part of a cam_drive pack: vision.bin.
//
// vision.bin is a little-endian binary file written by car/tools/train_pack.py
// (format 1, docs 4.4).  It holds the feature configuration, the standardisation
// of the 12 cell features, the detectors (linear models with thresholds and
// temporal parameters) and an optional self-test block (a small image with the
// expected features and scores) that the car verifies before using the pack.
//
//   0   "RCV1"            4   magic
//   4   u16 version       = 1
//   6   u16 feature_set   = 1 (vision_features.hpp)
//   8   u16 downscale     10 u16 grid_x   12 u16 grid_y
//   14  u8 s_min  15 u8 v_min  16 u8 v_white  17 u8 flags (bit 0: self-test present)
//   18  u16 n_detectors   20 u16 image_w  22 u16 image_h   (camera frame size the pack was made for)
//   24  f32 mean[12]      72 f32 std[12]
//   120 detectors: u8 kind, u8 name_len, name, u16 dims, f32 w[dims], f32 b, f32 thr_on, f32 thr_off,
//                  u16 confirm_frames, u16 release_frames, u16 min_cells, u8 row_min, u8 row_max
//   ... self-test (flag): u16 sw, u16 sh, u8 rgb[sw*sh*3], i32 features[dims], f32 scores[n_detectors]
//   end-4 u32 crc32 (IEEE, as zlib) of everything before it
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "vision_features.hpp"

namespace rumicar::vision {

constexpr uint16_t kPackVersion = 1;
constexpr size_t kPackMaxBytes = 16u * 1024 * 1024;
constexpr size_t kMaxDetectors = 32;
constexpr size_t kMaxNameLength = 32;

enum class DetectorKind : uint8_t { Global = 0, Local = 1 };

struct Detector {
    DetectorKind kind = DetectorKind::Global;
    std::string name;             // [a-z0-9_]{1,32}
    std::vector<float> weights;   // Global: dims() of the config; Local: kLocalDims
    float bias = 0.0f;
    float thr_on = 0.5f;          // score at or above which the detector is "present"
    float thr_off = 0.3f;         // once active, score below this counts towards release
    uint16_t confirm_frames = 3;  // consecutive present frames before it is active
    uint16_t release_frames = 5;  // consecutive absent frames before it is inactive
    uint16_t min_cells = 1;       // Local: cells at/above thr_on needed for "present"
    uint8_t row_min = 0;          // Local: rows of cells considered (inclusive)
    uint8_t row_max = 255;
};

struct SelfTest {
    bool present = false;
    SmallImage image;                 // the small (already downscaled) image
    std::vector<int32_t> features;    // expected features of that image
    std::vector<float> scores;        // expected score per detector (Local: max over its rows)
};

struct VisionPack {
    FeatureConfig config;
    unsigned image_w = 320;
    unsigned image_h = 240;
    float mean[kFeaturesPerCell] = {};
    float std[kFeaturesPerCell] = {};
    std::vector<Detector> detectors;
    SelfTest selftest;
};

// CRC-32 (IEEE 802.3 polynomial, reflected; same result as zlib.crc32).
uint32_t crc32(const uint8_t* data, size_t len, uint32_t seed = 0);

// Parses and validates a vision.bin image (sizes, ranges, names, CRC).  Throws
// std::runtime_error naming the first problem.
VisionPack parse_vision_pack(const std::vector<uint8_t>& bytes);

// Reads <dir>/vision.bin.  Throws std::runtime_error.
VisionPack load_vision_pack(const std::string& path);

// Serialises a pack (used by tests and tools; the CRC is appended).
std::vector<uint8_t> encode_vision_pack(const VisionPack& pack);

bool valid_detector_name(const std::string& name);

} // namespace rumicar::vision
