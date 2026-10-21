// RumiCar-Zero2Cam — camera-assisted driving logic (pure computation).
//
// The ToF driving logic (tof_drive) keeps doing what it does — avoiding walls
// and obstacles with smooth speed changes — and this class adds what the
// camera can tell.  The pack's detectors (vision.bin) say what is in view and
// three behaviours turn that into a speed cap and a steering mode for the ToF
// logic, so every stop, start and turn is as smooth as every other change:
//
//   signal        a red light: approach slowly, stop, hold, resume on green
//   vehicle       a vehicle ahead: follow it at a distance; when it does not
//                 move it is parked, so pass it (out - along - in) if a side is clear
//   intersection  an intersection: approach slowly, stop, then go straight or turn
//
// The behaviours are independent; where they meet, the lower speed cap wins,
// a manoeuvre (passing, turning) gives way to a red light, and the state
// reported is the one that explains what the car is doing (signal, then
// intersection, then vehicle).  The ToF logic's emergency brake, reversing
// recovery and sensor fault always win.  Without a vision.bin the logic is
// the ToF logic.
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

// State ids: 0..5 are the ToF logic's states (reported while no behaviour is
// active); the camera behaviours' states come after.
enum class State : uint8_t {
    SignalApproach = 16,       // a red light is in view but not close: speed capped at approach_v
    SignalStop = 17,           // stopped (or stopping) at a red light
    SignalResume = 18,         // moving off after the light; red is ignored for resume_ignore_ms
    Follow = 19,               // a vehicle ahead: holding the lane, keeping the gap
    FollowWait = 20,           // it is parked but no side is clear: waiting behind it
    OvertakeOut = 21,          // steering out to pass it
    OvertakePass = 22,         // alongside it
    OvertakeIn = 23,           // steering back in
    IntersectionApproach = 24, // an intersection is in view: speed capped at approach_v
    IntersectionStop = 25,     // stopped at it
    IntersectionCross = 26,    // moving on straight, waiting for the turn side to open
    IntersectionTurn = 27,     // turning
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
    // when the parameters name detectors the pack does not have, or of the
    // wrong kind, or lane_cols exceeds the pack's grid.
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
    enum class SignalPhase { Normal, Approach, Stop, Resume };
    enum class VehiclePhase { Normal, Follow, Wait, Out, Pass, In };
    enum class IntersectionPhase { Normal, Approach, Stop, Cross, Turn };

    CamParams p_;
    tof::TofDriveLogic tof_;
    std::unique_ptr<vision::VisionEvaluator> evaluator_;
    int red_ = -1;       // detector indices, -1 = none
    int green_ = -1;
    int vehicle_ = -1;
    int ileft_ = -1;
    int iright_ = -1;
    unsigned lane_col_min_ = 0, lane_col_max_ = 0;
    std::vector<int32_t> features_;
    bool first_ = true;

    // signal
    SignalPhase sphase_ = SignalPhase::Normal;
    uint64_t sphase_since_us_ = 0;
    uint64_t red_last_seen_us_ = 0;
    bool red_seen_ever_ = false;

    // vehicle
    VehiclePhase vphase_ = VehiclePhase::Normal;
    uint64_t vphase_since_us_ = 0;
    uint64_t vehicle_ignore_until_us_ = 0;
    uint64_t vehicle_lost_since_us_ = 0;   // 0 = not lost
    bool parked_timing_ = false;
    uint64_t parked_since_us_ = 0;
    int parked_ref_c_ = 0;                 // Follow: distance when the wait began; Wait: the distance to watch
    int8_t overtake_dir_ = RUMI_STEER_CENTER;
    bool pass_seen_ = false;               // the inner side ToF has seen the vehicle
    bool pass_clear_timing_ = false;
    uint64_t pass_clear_since_us_ = 0;

    // intersection
    IntersectionPhase iphase_ = IntersectionPhase::Normal;
    uint64_t iphase_since_us_ = 0;
    uint64_t intersection_ignore_until_us_ = 0;
    int8_t turn_dir_ = RUMI_STEER_CENTER;  // decided when stopping: CENTER = straight

    uint8_t state_ = 0;

    struct View {   // what the frame's detections and the (previous frame's) conditioned ToF say
        bool red_active = false, red_close = false, green_active = false;
        bool vehicle_tracked = false;   // the vehicle detector is active with its best cell in our lane
        bool ileft_active = false, iright_active = false;
        int L = 0, C = 0, R = 0;
    };
    View look() const;
    void step_signal(const View& v, uint64_t t);
    void step_intersection(const View& v, uint64_t t);
    void step_vehicle(const View& v, uint64_t t);
    int8_t choose_overtake_side(int L, int R) const;
    int8_t decide_turn(const View& v) const;
    int follow_cap(int C) const;
    bool tof_driving() const;
    void fill_aux(rumi_command& out) const;
};

} // namespace rumicar::cam
