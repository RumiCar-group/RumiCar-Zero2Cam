#include "vision_eval.hpp"

#include <algorithm>
#include <cmath>

namespace rumicar::vision {

namespace {

float sigmoid(float logit) {
    return static_cast<float>(1.0 / (1.0 + std::exp(-static_cast<double>(logit))));
}

} // namespace

float global_score(const VisionPack& p, const Detector& d, const std::vector<int32_t>& features) {
    float acc = d.bias;
    const size_t n = std::min(d.weights.size(), features.size());
    for (size_t k = 0; k < n; ++k) {
        acc += d.weights[k] * standardise(p, features[k], static_cast<unsigned>(k % kFeaturesPerCell));
    }
    return sigmoid(acc);
}

float local_score(const VisionPack& p, const Detector& d, const std::vector<int32_t>& features, unsigned cx, unsigned cy) {
    const unsigned gx = p.config.grid_x, gy = p.config.grid_y;
    float acc = d.bias;
    size_t k = 0;
    for (int dj = -1; dj <= 1; ++dj) {
        for (int di = -1; di <= 1; ++di) {
            const int nx = static_cast<int>(cx) + di, ny = static_cast<int>(cy) + dj;
            if (nx >= 0 && ny >= 0 && nx < static_cast<int>(gx) && ny < static_cast<int>(gy)) {
                const size_t base = (static_cast<size_t>(ny) * gx + static_cast<size_t>(nx)) * kFeaturesPerCell;
                for (unsigned t = 0; t < kFeaturesPerCell; ++t) {
                    acc += d.weights[k + t] * standardise(p, features[base + t], t);
                }
            }
            // outside the grid: standardised value 0 contributes nothing
            k += kFeaturesPerCell;
        }
    }
    return sigmoid(acc);
}

DetectorOutput evaluate_detector(const VisionPack& p, const Detector& d, const std::vector<int32_t>& features) {
    DetectorOutput o;
    if (d.kind == DetectorKind::Global) {
        o.score = global_score(p, d, features);
        o.present = o.score >= d.thr_on;
        return o;
    }
    const unsigned gx = p.config.grid_x, gy = p.config.grid_y;
    const unsigned row_end = std::min<unsigned>(gy - 1, d.row_max);
    bool first = true;
    for (unsigned cy = d.row_min; cy <= row_end; ++cy) {
        for (unsigned cx = 0; cx < gx; ++cx) {
            const float s = local_score(p, d, features, cx, cy);
            if (first || s > o.score) {   // the first best cell in row-major order wins ties
                o.score = s;
                o.cx = static_cast<uint8_t>(cx);
                o.cy = static_cast<uint8_t>(cy);
                first = false;
            }
            if (s >= d.thr_on) ++o.cells;
        }
    }
    o.present = !first && o.score >= d.thr_on && o.cells >= d.min_cells;
    return o;
}

VisionEvaluator::VisionEvaluator(VisionPack pack) : pack_(std::move(pack)) {
    state_.assign(pack_.detectors.size(), Temporal{});
    out_.assign(pack_.detectors.size(), DetectorOutput{});
}

void VisionEvaluator::reset() {
    for (auto& t : state_) t = Temporal{};
    for (auto& o : out_) o = DetectorOutput{};
}

int VisionEvaluator::find(const std::string& name) const {
    for (size_t i = 0; i < pack_.detectors.size(); ++i) {
        if (pack_.detectors[i].name == name) return static_cast<int>(i);
    }
    return -1;
}

const std::vector<DetectorOutput>& VisionEvaluator::step(const std::vector<int32_t>& features) {
    for (size_t i = 0; i < pack_.detectors.size(); ++i) {
        const Detector& d = pack_.detectors[i];
        DetectorOutput o = evaluate_detector(pack_, d, features);
        Temporal& t = state_[i];
        if (!t.active) {
            if (o.present) {
                if (t.above < 65535) ++t.above;
            } else {
                t.above = 0;
            }
            if (t.above >= d.confirm_frames) {
                t.active = true;
                t.below = 0;
            }
        } else {
            // Hysteresis: the (best) score may sag to thr_off before release counts.
            const bool still = o.score >= d.thr_off;
            if (still) {
                t.below = 0;
            } else {
                if (t.below < 65535) ++t.below;
            }
            if (t.below >= d.release_frames) {
                t.active = false;
                t.above = 0;
            }
        }
        o.active = t.active;
        out_[i] = o;
    }
    return out_;
}

} // namespace rumicar::vision
