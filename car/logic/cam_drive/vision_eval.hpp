// RumiCar-Zero2Cam — applying a vision pack's detectors to one frame's
// features, with the temporal confirmation that turns per-frame scores into
// a stable "present / absent" decision (docs 4.4).  Pure computation.
#pragma once

#include <cstdint>
#include <vector>

#include "vision_pack.hpp"

namespace rumicar::vision {

struct DetectorOutput {
    float score = 0.0f;      // Global: the score; Local: the best cell's score within the rows
    uint8_t cx = 0;          // Local: column of the best cell
    uint8_t cy = 0;          // Local: row of the best cell
    uint16_t cells = 0;      // Local: cells at/above thr_on within the rows (0 for Global)
    bool present = false;    // this frame: score >= thr_on (and cells >= min_cells for Local)
    bool active = false;     // after confirmation / before release
};

// Standardised value of feature k (k = cell*12 + type) with the pack's statistics.
inline float standardise(const VisionPack& p, int32_t value, unsigned type) {
    return (static_cast<float>(value) - p.mean[type]) / p.std[type];
}

// Score of one Global detector on a feature vector (float accumulation in index order).
float global_score(const VisionPack& p, const Detector& d, const std::vector<int32_t>& features);

// Score of one Local detector at cell (cx, cy): its 3×3 neighbourhood,
// neighbours outside the grid contribute 0 (the standardised mean).
float local_score(const VisionPack& p, const Detector& d, const std::vector<int32_t>& features, unsigned cx, unsigned cy);

// Per-frame output of a detector without temporal state.
DetectorOutput evaluate_detector(const VisionPack& p, const Detector& d, const std::vector<int32_t>& features);

class VisionEvaluator {
public:
    explicit VisionEvaluator(VisionPack pack);

    const VisionPack& pack() const { return pack_; }

    // Computes every detector's output for the frame and advances the
    // confirmation/release counters.  Returns one entry per detector, in
    // pack order.
    const std::vector<DetectorOutput>& step(const std::vector<int32_t>& features);

    // Resets the temporal state (a new drive).
    void reset();

    // Index of the detector with this name, or -1.
    int find(const std::string& name) const;

    const std::vector<DetectorOutput>& last() const { return out_; }

private:
    struct Temporal {
        uint16_t above = 0;
        uint16_t below = 0;
        bool active = false;
    };
    VisionPack pack_;
    std::vector<Temporal> state_;
    std::vector<DetectorOutput> out_;
};

} // namespace rumicar::vision
