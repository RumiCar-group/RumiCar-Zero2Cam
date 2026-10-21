#include "vision_features.hpp"

#include <algorithm>
#include <cstdlib>

namespace rumicar::vision {

std::string validate_config(const FeatureConfig& c, unsigned w, unsigned h) {
    if (c.downscale < 1 || c.downscale > 16) return "downscale must be 1..16";
    if (c.grid_x < 1 || c.grid_x > 64 || c.grid_y < 1 || c.grid_y > 64) return "grid must be 1..64 cells";
    if (w == 0 || h == 0) return "image size must be positive";
    const unsigned sw = w / c.downscale, sh = h / c.downscale;
    if (sw / c.grid_x < 2 || sh / c.grid_y < 2) return "cells must be at least 2x2 small pixels";
    return {};
}

SmallImage downscale(const uint8_t* rgb, unsigned width, unsigned height, unsigned stride, unsigned factor) {
    SmallImage s;
    if (factor == 0) return s;
    s.width = width / factor;
    s.height = height / factor;
    s.rgb.assign(static_cast<size_t>(s.width) * s.height * 3, 0);
    const unsigned n = factor * factor;
    for (unsigned y = 0; y < s.height; ++y) {
        for (unsigned x = 0; x < s.width; ++x) {
            uint32_t sum[3] = {0, 0, 0};
            for (unsigned dy = 0; dy < factor; ++dy) {
                const uint8_t* row = rgb + static_cast<size_t>(y * factor + dy) * stride + static_cast<size_t>(x) * factor * 3;
                for (unsigned dx = 0; dx < factor; ++dx) {
                    sum[0] += row[dx * 3];
                    sum[1] += row[dx * 3 + 1];
                    sum[2] += row[dx * 3 + 2];
                }
            }
            uint8_t* o = s.rgb.data() + (static_cast<size_t>(y) * s.width + x) * 3;
            o[0] = static_cast<uint8_t>(sum[0] / n);
            o[1] = static_cast<uint8_t>(sum[1] / n);
            o[2] = static_cast<uint8_t>(sum[2] / n);
        }
    }
    return s;
}

unsigned hue_degrees(uint8_t r, uint8_t g, uint8_t b) {
    const unsigned mx = std::max({r, g, b});
    const unsigned mn = std::min({r, g, b});
    const unsigned d = mx - mn;   // caller guarantees d > 0
    // Sector by the largest channel, then the position inside it; all terms are
    // non-negative so integer division is the same floor in every language.
    if (mx == r) {
        if (g >= b) return 60u * (g - b) / d;              // 0..60   red -> yellow
        return 360u - 60u * (b - g) / d;                   // 300..359 magenta -> red
    }
    if (mx == g) {
        if (b >= r) return 120u + 60u * (b - r) / d;       // 120..180 green -> cyan
        return 120u - 60u * (r - b) / d;                   // 60..120  yellow -> green
    }
    if (r >= g) return 240u + 60u * (r - g) / d;           // 240..300 blue -> magenta
    return 240u - 60u * (g - r) / d;                       // 180..240 cyan -> blue
}

PixelClass classify(uint8_t r, uint8_t g, uint8_t b, const FeatureConfig& c) {
    const unsigned mx = std::max({r, g, b});
    const unsigned mn = std::min({r, g, b});
    const unsigned d = mx - mn;
    const unsigned s = mx == 0 ? 0 : d * 255u / mx;
    if (d > 0 && s >= c.s_min && mx >= c.v_min) {
        const unsigned h = hue_degrees(r, g, b);
        return static_cast<PixelClass>(((h + 30u) % 360u) / 60u);
    }
    return mx >= c.v_white ? Bright : Dark;
}

void compute_features(const SmallImage& small, const FeatureConfig& c, std::vector<int32_t>& out,
                      unsigned* cell_w, unsigned* cell_h) {
    const unsigned sw = small.width, sh = small.height;
    const unsigned cw = sw / c.grid_x, ch = sh / c.grid_y;
    if (cell_w) *cell_w = cw;
    if (cell_h) *cell_h = ch;
    out.assign(c.dims(), 0);
    if (cw == 0 || ch == 0) return;

    // Per-pixel class, V and S, then gradients of V.
    const size_t n = static_cast<size_t>(sw) * sh;
    std::vector<uint8_t> cls(n), v(n), s(n);
    for (size_t i = 0; i < n; ++i) {
        const uint8_t r = small.rgb[i * 3], g = small.rgb[i * 3 + 1], b = small.rgb[i * 3 + 2];
        const unsigned mx = std::max({r, g, b});
        const unsigned mn = std::min({r, g, b});
        v[i] = static_cast<uint8_t>(mx);
        s[i] = static_cast<uint8_t>(mx == 0 ? 0 : (mx - mn) * 255u / mx);
        cls[i] = classify(r, g, b, c);
    }
    for (unsigned j = 0; j < c.grid_y; ++j) {
        for (unsigned i = 0; i < c.grid_x; ++i) {
            int32_t* f = out.data() + (static_cast<size_t>(j) * c.grid_x + i) * kFeaturesPerCell;
            for (unsigned y = j * ch; y < (j + 1) * ch; ++y) {
                for (unsigned x = i * cw; x < (i + 1) * cw; ++x) {
                    const size_t p = static_cast<size_t>(y) * sw + x;
                    f[cls[p]] += 1;
                    f[8] += v[p];
                    f[9] += s[p];
                    const int dx = x + 1 < sw ? std::abs(static_cast<int>(v[p + 1]) - static_cast<int>(v[p])) : 0;
                    const int dy = y + 1 < sh ? std::abs(static_cast<int>(v[p + sw]) - static_cast<int>(v[p])) : 0;
                    f[10] += dx;
                    f[11] += dy;
                }
            }
        }
    }
}

void extract_features(const uint8_t* rgb, unsigned width, unsigned height, unsigned stride, const FeatureConfig& c,
                      std::vector<int32_t>& out) {
    const SmallImage small = downscale(rgb, width, height, stride, c.downscale);
    compute_features(small, c, out);
}

} // namespace rumicar::vision
