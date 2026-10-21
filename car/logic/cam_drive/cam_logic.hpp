// RumiCar-Zero2Cam — camera-assisted driving logic (pure computation).
//
// The ToF driving logic (tof_drive) keeps doing what it does — avoiding walls
// and obstacles with smooth speed changes — and this class adds what the
// camera can tell: in this version, traffic lights.  The pack's detectors
// (vision.bin) say whether a red or green light is in view; the behaviour
// below turns that into a speed cap on the ToF logic (approach slowly, stop,
// hold, resume), so stopping and starting are as smooth as every other speed
// change.  Without a vision.bin the logic is the ToF logic.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "cam_params.hpp"
#include "rumicar/logic_api.h"
#include "tof_logic.hpp"
#include "vision_eval.hpp"

namespace rumicar::cam {

// State ids: 0..5 are the ToF logic's states (reported while no signal
// behaviour is active); the signal states come after.
enum class State : uint8_t {
    SignalApproach = 16,   // a red light is in view but not close: speed capped at approach_v
    SignalStop = 17,       // stopped (or stopping) at a red light
    SignalResume = 18,     // moving off after the light; red is ignored for resume_ignore_ms
};

const char* state_name(uint8_t state);

// Aux output format 1 (rumi_command::aux_format): [grid_x, grid_y] then 5
// bytes per detector in pack order, at most kAuxDetectors of them:
// score (0..255), cx, cy, cells (clamped to 255), flags (bit 0 present, bit 1 active).
constexpr uint16_t kAuxFormat = 1;
constexpr unsigned kAuxDetectorBytes = 5;
constexpr unsigned kAuxDetectors = (RUMI_AUX_BYTES - 2) / kAuxDetectorBytes;   // 9

class CamLogic {
public:
    // The vision pack is optional (ToF-only pack).  Throws std::runtime_error
    // when the signal parameters name detectors the pack does not have.
    CamLogic(const CamParams& params, std::optional<vision::VisionPack> pack);

    // Process one frame.  Returns 0 on success; 2 for a malformed frame; 3
    // when the image size differs from what the pack was made for.
    int step(const rumi_sensor_frame& frame, rumi_command& out);

    bool has_vision() const { return evaluator_ != nullptr; }
    const std::vector<vision::DetectorOutput>* detections() const { return evaluator_ ? &evaluator_->last() : nullptr; }
    const vision::VisionPack* pack() const { return evaluator_ ? &evaluator_->pack() : nullptr; }
    uint8_t state() const { return state_; }
    const tof::TofDriveLogic& tof() const { return tof_; }

    // Name of the aux slot (detector) `index`, or nullptr.
    const char* aux_name(uint8_t index) const;

private:
    enum class Phase { Normal, Approach, Stop, Resume };

    CamParams p_;
    tof::TofDriveLogic tof_;
    std::unique_ptr<vision::VisionEvaluator> evaluator_;
    int red_ = -1;     // detector indices, -1 = none
    int green_ = -1;
    std::vector<int32_t> features_;

    Phase phase_ = Phase::Normal;
    uint64_t phase_since_us_ = 0;
    uint64_t red_last_seen_us_ = 0;
    bool red_seen_ever_ = false;
    uint8_t state_ = 0;

    void fill_aux(rumi_command& out) const;
};

} // namespace rumicar::cam
