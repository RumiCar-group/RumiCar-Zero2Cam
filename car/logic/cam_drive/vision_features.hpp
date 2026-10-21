// RumiCar-Zero2Cam — image features of the camera driving logic (feature set 1).
//
// The camera image is reduced to a small grid of integer statistics that a
// learned detector (vision_pack.hpp) is applied to.  Every step is integer
// arithmetic with a fixed definition, so that the Python implementation used
// for training (car/tools/rumicar_vision.py) and this one produce exactly the
// same numbers; car/tests/test_vision.cpp checks that against golden data.
//
//   RGB24 W×H ──(1) box average by `downscale`──> small image sw×sh
//   ──(2) per pixel: V = max, S, hue bin or bright/dark class, |dV/dx|, |dV/dy|
//   ──(3) per cell of a grid_x×grid_y grid: 8 class counts, ΣV, ΣS, Σ|dx|, Σ|dy|
//   = 12 integers per cell, grid_y×grid_x×12 integers in row-major cell order.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rumicar::vision {

constexpr unsigned kFeatureSet = 1;
constexpr unsigned kFeaturesPerCell = 12;
constexpr unsigned kClassCount = 8;          // 6 hue bins + bright + dark
constexpr unsigned kLocalWindow = 3;         // 3×3 cells around a cell
constexpr unsigned kLocalDims = kLocalWindow * kLocalWindow * kFeaturesPerCell;   // 108

// Hue bins (60° each, centred on the primaries): red, yellow, green, cyan, blue, magenta.
enum PixelClass : uint8_t { Red = 0, Yellow = 1, Green = 2, Cyan = 3, Blue = 4, Magenta = 5, Bright = 6, Dark = 7 };

struct FeatureConfig {
    unsigned downscale = 5;   // 1..16: 320×240 -> 64×48
    unsigned grid_x = 8;      // 1..64 cells across
    unsigned grid_y = 6;      // 1..64 cells down
    uint8_t s_min = 64;       // saturation (0..255) at or above which a pixel has a hue
    uint8_t v_min = 48;       // brightness (0..255) at or above which a pixel has a hue
    uint8_t v_white = 160;    // unsaturated pixels at or above this are "bright", below "dark"

    unsigned dims() const { return grid_x * grid_y * kFeaturesPerCell; }
};

// Validates the configuration for an image of w×h.  Returns an empty string
// when usable: every cell must be at least 2×2 small pixels.
std::string validate_config(const FeatureConfig& c, unsigned w, unsigned h);

struct SmallImage {
    unsigned width = 0;
    unsigned height = 0;
    std::vector<uint8_t> rgb;   // tightly packed RGB24
};

// (1) Box average: each output pixel is the integer mean (floor) of a
// downscale×downscale block; right/bottom remainders are dropped.
SmallImage downscale(const uint8_t* rgb, unsigned width, unsigned height, unsigned stride, unsigned factor);

// Hue in degrees 0..359 for a pixel with max > min (integer formulas, docs 3.10).
unsigned hue_degrees(uint8_t r, uint8_t g, uint8_t b);

// (2) Class of one pixel.
PixelClass classify(uint8_t r, uint8_t g, uint8_t b, const FeatureConfig& c);

// (3) Cell features of a small image.  `out` is resized to c.dims().  Returns
// the cell size (cw, ch) used.  The small image must satisfy validate_config
// for the original image size it came from.
void compute_features(const SmallImage& small, const FeatureConfig& c, std::vector<int32_t>& out,
                      unsigned* cell_w = nullptr, unsigned* cell_h = nullptr);

// Convenience: full pipeline from an RGB24 frame.
void extract_features(const uint8_t* rgb, unsigned width, unsigned height, unsigned stride, const FeatureConfig& c,
                      std::vector<int32_t>& out);

} // namespace rumicar::vision
