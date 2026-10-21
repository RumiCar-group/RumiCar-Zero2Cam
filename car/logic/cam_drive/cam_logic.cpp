#include "cam_logic.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "vision_features.hpp"

namespace rumicar::cam {

namespace {

constexpr uint64_t ms_to_us(int ms) { return static_cast<uint64_t>(ms) * static_cast<uint64_t>(1000); }

bool tof_is_driving_state(uint8_t s) {
    return s == static_cast<uint8_t>(tof::State::Cruise) || s == static_cast<uint8_t>(tof::State::TurnLeft) ||
           s == static_cast<uint8_t>(tof::State::TurnRight);
}

} // namespace

const char* state_name(uint8_t state) {
    switch (static_cast<State>(state)) {
    case State::SignalApproach: return "SIGNAL_APPROACH";
    case State::SignalStop:     return "SIGNAL_STOP";
    case State::SignalResume:   return "SIGNAL_RESUME";
    }
    return tof::state_name(static_cast<tof::State>(state));
}

CamLogic::CamLogic(const CamParams& params, std::optional<vision::VisionPack> pack) : p_(params), tof_(params.tof) {
    if (pack) {
        evaluator_ = std::make_unique<vision::VisionEvaluator>(std::move(*pack));
        if (!p_.signal.red_detector.empty()) {
            red_ = evaluator_->find(p_.signal.red_detector);
            if (red_ < 0) throw std::runtime_error("vision.bin has no detector named '" + p_.signal.red_detector + "' (signal.red_detector)");
            if (evaluator_->pack().detectors[static_cast<size_t>(red_)].kind != vision::DetectorKind::Local)
                throw std::runtime_error("signal.red_detector must be a local detector");
        }
        if (!p_.signal.green_detector.empty()) {
            green_ = evaluator_->find(p_.signal.green_detector);
            if (green_ < 0) throw std::runtime_error("vision.bin has no detector named '" + p_.signal.green_detector + "' (signal.green_detector)");
        }
    }
}

const char* CamLogic::aux_name(uint8_t index) const {
    if (!evaluator_) return nullptr;
    const auto& dets = evaluator_->pack().detectors;
    if (index >= dets.size() || index >= kAuxDetectors) return nullptr;
    return dets[index].name.c_str();
}

void CamLogic::fill_aux(rumi_command& out) const {
    out.aux_format = 0;
    out.aux_len = 0;
    if (!evaluator_) return;
    const auto& pack = evaluator_->pack();
    const auto& dets = evaluator_->last();
    const unsigned n = static_cast<unsigned>(std::min<size_t>(dets.size(), kAuxDetectors));
    out.aux[0] = static_cast<uint8_t>(pack.config.grid_x);
    out.aux[1] = static_cast<uint8_t>(pack.config.grid_y);
    for (unsigned i = 0; i < n; ++i) {
        const auto& d = dets[i];
        uint8_t* a = out.aux + 2 + i * kAuxDetectorBytes;
        a[0] = static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(d.score * 255.0f)), 0, 255));
        a[1] = d.cx;
        a[2] = d.cy;
        a[3] = static_cast<uint8_t>(std::min<unsigned>(d.cells, 255));
        a[4] = static_cast<uint8_t>((d.present ? 1u : 0u) | (d.active ? 2u : 0u));
    }
    out.aux_format = kAuxFormat;
    out.aux_len = static_cast<uint8_t>(2 + n * kAuxDetectorBytes);
}

int CamLogic::step(const rumi_sensor_frame& frame, rumi_command& out) {
    if (frame.size < sizeof(rumi_sensor_frame)) return 2;
    const uint64_t t = frame.t_us;
    if (phase_since_us_ == 0) phase_since_us_ = t;

    // --- perception: only when a new image accompanies this frame ----------------
    if (evaluator_ && frame.image) {
        const auto& pack = evaluator_->pack();
        if (frame.image_width != pack.image_w || frame.image_height != pack.image_h) return 3;
        vision::extract_features(frame.image, frame.image_width, frame.image_height, frame.image_stride, pack.config, features_);
        evaluator_->step(features_);
    }
    bool red_active = false, red_close = false, green_active = false;
    if (evaluator_) {
        const auto& dets = evaluator_->last();
        if (red_ >= 0) {
            const auto& r = dets[static_cast<size_t>(red_)];
            red_active = r.active;
            red_close = red_active && r.cells >= static_cast<unsigned>(p_.signal.stop_min_cells);
        }
        if (green_ >= 0) green_active = dets[static_cast<size_t>(green_)].active;
    }
    if (red_active) {
        red_last_seen_us_ = t;
        red_seen_ever_ = true;
    }

    // --- behaviour ----------------------------------------------------------------
    auto enter = [&](Phase ph) {
        phase_ = ph;
        phase_since_us_ = t;
    };
    switch (phase_) {
    case Phase::Normal:
        if (red_close) enter(Phase::Stop);
        else if (red_active) enter(Phase::Approach);
        break;
    case Phase::Approach:
        if (red_close) enter(Phase::Stop);
        else if (!red_active) enter(Phase::Normal);
        break;
    case Phase::Stop:
        if (t - phase_since_us_ >= ms_to_us(p_.signal.min_stop_ms)) {
            const bool timed_out = p_.signal.resume_without_green_ms > 0 && !red_active && red_seen_ever_ &&
                                   (t - red_last_seen_us_) >= ms_to_us(p_.signal.resume_without_green_ms);
            if (green_active || timed_out) enter(Phase::Resume);
        }
        break;
    case Phase::Resume:
        if (t - phase_since_us_ >= ms_to_us(p_.signal.resume_ignore_ms)) enter(Phase::Normal);
        break;
    }
    int cap = RUMI_DRIVE_MAX;
    if (phase_ == Phase::Approach) cap = p_.signal.approach_v;
    else if (phase_ == Phase::Stop) cap = 0;
    tof_.set_speed_cap(cap);

    // --- the ToF logic drives, within the cap ----------------------------------------
    if (!tof_.step(frame, out)) return 2;
    const uint8_t tof_state = out.state;
    if (phase_ == Phase::Stop && out.drive_permille == 0 && tof_is_driving_state(tof_state)) {
        out.flags |= RUMI_CMD_FLAG_BRAKE;   // hold still at the light
    }
    if (phase_ != Phase::Normal && tof_is_driving_state(tof_state)) {
        out.state = static_cast<uint8_t>(phase_ == Phase::Approach ? State::SignalApproach
                                         : phase_ == Phase::Stop   ? State::SignalStop
                                                                   : State::SignalResume);
    }
    state_ = out.state;
    fill_aux(out);
    return 0;
}

} // namespace rumicar::cam
