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

constexpr int8_t kLeft = RUMI_STEER_LEFT, kCenter = RUMI_STEER_CENTER, kRight = RUMI_STEER_RIGHT;

int8_t opposite(int8_t steer) { return steer == kLeft ? kRight : steer == kRight ? kLeft : kCenter; }

} // namespace

const char* state_name(uint8_t state) {
    switch (static_cast<State>(state)) {
    case State::SignalApproach:       return "SIGNAL_APPROACH";
    case State::SignalStop:           return "SIGNAL_STOP";
    case State::SignalResume:         return "SIGNAL_RESUME";
    case State::Follow:               return "FOLLOW";
    case State::FollowWait:           return "FOLLOW_WAIT";
    case State::OvertakeOut:          return "OVERTAKE_OUT";
    case State::OvertakePass:         return "OVERTAKE_PASS";
    case State::OvertakeIn:           return "OVERTAKE_IN";
    case State::IntersectionApproach: return "INTERSECTION_APPROACH";
    case State::IntersectionStop:     return "INTERSECTION_STOP";
    case State::IntersectionCross:    return "INTERSECTION_CROSS";
    case State::IntersectionTurn:     return "INTERSECTION_TURN";
    }
    return tof::state_name(static_cast<tof::State>(state));
}

CamLogic::CamLogic(const CamParams& params, std::optional<vision::VisionPack> pack) : p_(params), tof_(params.tof) {
    if (!pack) return;
    evaluator_ = std::make_unique<vision::VisionEvaluator>(std::move(*pack));
    const auto& dets = evaluator_->pack().detectors;
    auto need = [&](const std::string& name, const char* key, bool local) {
        if (name.empty()) return -1;
        const int idx = evaluator_->find(name);
        if (idx < 0) throw std::runtime_error("vision.bin has no detector named '" + name + "' (" + key + ")");
        if (local && dets[static_cast<size_t>(idx)].kind != vision::DetectorKind::Local)
            throw std::runtime_error(std::string(key) + " must be a local detector");
        return idx;
    };
    red_ = need(p_.signal.red_detector, "signal.red_detector", true);
    green_ = need(p_.signal.green_detector, "signal.green_detector", false);
    if (p_.vehicle) {
        vehicle_ = need(p_.vehicle->detector, "vehicle.detector", true);
        const unsigned gx = evaluator_->pack().config.grid_x;
        if (static_cast<unsigned>(p_.vehicle->lane_cols) > gx)
            throw std::runtime_error("vehicle.lane_cols exceeds the pack's grid width " + std::to_string(gx));
        lane_col_min_ = (gx - static_cast<unsigned>(p_.vehicle->lane_cols)) / 2;
        lane_col_max_ = lane_col_min_ + static_cast<unsigned>(p_.vehicle->lane_cols) - 1;
    }
    if (p_.intersection) {
        ileft_ = need(p_.intersection->left_detector, "intersection.left_detector", false);
        iright_ = need(p_.intersection->right_detector, "intersection.right_detector", false);
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

bool CamLogic::tof_driving() const { return tof_is_driving_state(static_cast<uint8_t>(tof_.state())); }

// ---------------------------------------------------------------------------
// What this frame shows.  The ToF distances are the conditioned values of the
// previous frame (the ToF logic conditions this frame's readings when it
// steps, after the behaviours have decided): one frame of lag, which every
// decision below tolerates since each is confirmed over time.
// ---------------------------------------------------------------------------
CamLogic::View CamLogic::look() const {
    View v;
    const auto& f = tof_.filtered();
    v.L = f[RUMI_TOF_LEFT];
    v.C = f[RUMI_TOF_CENTER];
    v.R = f[RUMI_TOF_RIGHT];
    if (!evaluator_) return v;
    const auto& dets = evaluator_->last();
    if (dets.empty()) return v;   // no image yet
    if (red_ >= 0) {
        const auto& r = dets[static_cast<size_t>(red_)];
        v.red_active = r.active;
        v.red_close = r.active && r.cells >= static_cast<unsigned>(p_.signal.stop_min_cells);
    }
    if (green_ >= 0) v.green_active = dets[static_cast<size_t>(green_)].active;
    if (vehicle_ >= 0) {
        const auto& d = dets[static_cast<size_t>(vehicle_)];
        v.vehicle_tracked = d.active && d.cx >= lane_col_min_ && d.cx <= lane_col_max_;
    }
    if (ileft_ >= 0) v.ileft_active = dets[static_cast<size_t>(ileft_)].active;
    if (iright_ >= 0) v.iright_active = dets[static_cast<size_t>(iright_)].active;
    return v;
}

// ---------------------------------------------------------------------------
// Traffic lights
// ---------------------------------------------------------------------------
void CamLogic::step_signal(const View& v, uint64_t t) {
    if (v.red_active) {
        red_last_seen_us_ = t;
        red_seen_ever_ = true;
    }
    auto enter = [&](SignalPhase ph) {
        sphase_ = ph;
        sphase_since_us_ = t;
    };
    switch (sphase_) {
    case SignalPhase::Normal:
        if (v.red_close) enter(SignalPhase::Stop);
        else if (v.red_active) enter(SignalPhase::Approach);
        break;
    case SignalPhase::Approach:
        if (v.red_close) enter(SignalPhase::Stop);
        else if (!v.red_active) enter(SignalPhase::Normal);
        break;
    case SignalPhase::Stop:
        if (t - sphase_since_us_ >= ms_to_us(p_.signal.min_stop_ms)) {
            const bool timed_out = p_.signal.resume_without_green_ms > 0 && !v.red_active && red_seen_ever_ &&
                                   (t - red_last_seen_us_) >= ms_to_us(p_.signal.resume_without_green_ms);
            if (v.green_active || timed_out) enter(SignalPhase::Resume);
        }
        break;
    case SignalPhase::Resume:
        if (t - sphase_since_us_ >= ms_to_us(p_.signal.resume_ignore_ms)) enter(SignalPhase::Normal);
        break;
    }
}

// ---------------------------------------------------------------------------
// Intersections
// ---------------------------------------------------------------------------
int8_t CamLogic::decide_turn(const View& v) const {
    const std::string& turn = p_.intersection->turn;
    if (turn == "left") return kLeft;
    if (turn == "right") return kRight;
    if (turn == "detector") {
        if (v.ileft_active && !v.iright_active) return kLeft;
        if (v.iright_active && !v.ileft_active) return kRight;
    }
    return kCenter;   // "straight", or a crossroads seen both ways
}

void CamLogic::step_intersection(const View& v, uint64_t t) {
    if (!p_.intersection) return;
    const IntersectionParams& ip = *p_.intersection;
    const bool seen = v.ileft_active || v.iright_active;
    const bool side_open = v.L >= ip.open_mm || v.R >= ip.open_mm;
    const bool red = sphase_ == SignalPhase::Approach || sphase_ == SignalPhase::Stop;
    auto enter = [&](IntersectionPhase ph) {
        iphase_ = ph;
        iphase_since_us_ = t;
    };
    auto leave = [&] {
        iphase_ = IntersectionPhase::Normal;
        intersection_ignore_until_us_ = t + ms_to_us(ip.ignore_ms);
    };
    const bool overtaking = vphase_ == VehiclePhase::Out || vphase_ == VehiclePhase::Pass || vphase_ == VehiclePhase::In;
    switch (iphase_) {
    case IntersectionPhase::Normal:
        if (seen && t >= intersection_ignore_until_us_ && !overtaking) {
            if (ip.stop_trigger == "detector") {
                turn_dir_ = decide_turn(v);
                enter(IntersectionPhase::Stop);
            } else {
                enter(IntersectionPhase::Approach);
            }
        }
        break;
    case IntersectionPhase::Approach:
        if (!seen) {
            iphase_ = IntersectionPhase::Normal;   // lost from view: nothing happened, no ignore time
        } else if (side_open) {
            turn_dir_ = decide_turn(v);
            enter(IntersectionPhase::Stop);
        }
        break;
    case IntersectionPhase::Stop:
        // Stay at least stop_ms, and never move off into a red light.
        if (t - iphase_since_us_ >= ms_to_us(ip.stop_ms) && !red) {
            if (turn_dir_ == RUMI_STEER_CENTER) leave();
            else enter(IntersectionPhase::Cross);
        }
        break;
    case IntersectionPhase::Cross: {
        const int side = turn_dir_ == RUMI_STEER_LEFT ? v.L : v.R;
        if (red || !tof_driving()) leave();
        else if (side >= ip.open_mm) enter(IntersectionPhase::Turn);
        else if (t - iphase_since_us_ >= ms_to_us(ip.cross_max_ms)) leave();   // the turn side never opened: go on straight
        break;
    }
    case IntersectionPhase::Turn:
        if (red || !tof_driving() || t - iphase_since_us_ >= ms_to_us(ip.turn_ms)) leave();
        break;
    }
}

// ---------------------------------------------------------------------------
// Vehicles ahead
// ---------------------------------------------------------------------------
int CamLogic::follow_cap(int C) const {
    const VehicleParams& vp = *p_.vehicle;
    if (C >= tof::TofDriveLogic::OPEN_MM) return vp.follow_v;   // seen by the camera only: not close yet
    if (C <= vp.follow_gap_mm) return 0;
    if (C >= vp.follow_gap_mm + vp.follow_band_mm) return vp.follow_v;
    return static_cast<int>(std::lround(static_cast<double>(vp.follow_v) * (C - vp.follow_gap_mm) / vp.follow_band_mm));
}

int8_t CamLogic::choose_overtake_side(int L, int R) const {
    const VehicleParams& vp = *p_.vehicle;
    const bool l_ok = L >= vp.side_clear_mm;
    const bool r_ok = R >= vp.side_clear_mm;
    if (vp.overtake_side == "left") return l_ok ? kLeft : kCenter;
    if (vp.overtake_side == "right") return r_ok ? kRight : kCenter;
    if (l_ok && r_ok) return L > R ? kLeft : kRight;   // the roomier side; equal -> right
    return l_ok ? kLeft : r_ok ? kRight : kCenter;
}

void CamLogic::step_vehicle(const View& v, uint64_t t) {
    if (!p_.vehicle) return;
    const VehicleParams& vp = *p_.vehicle;
    const bool near = v.C < tof::TofDriveLogic::OPEN_MM;   // the ToF sees something ahead
    const bool busy = sphase_ != SignalPhase::Normal || iphase_ != IntersectionPhase::Normal;
    auto enter = [&](VehiclePhase ph) {
        vphase_ = ph;
        vphase_since_us_ = t;
        parked_timing_ = false;
    };
    auto leave = [&] {   // after a manoeuvre, or one that was cut short
        vphase_ = VehiclePhase::Normal;
        vehicle_ignore_until_us_ = t + ms_to_us(vp.ignore_ms);
        parked_timing_ = false;
    };
    // The vehicle out of the camera's sight (too close to be recognised, or
    // gone) for ignore_ms while following: back to the ToF logic, which turns
    // around whatever is there.
    if (v.vehicle_tracked) vehicle_lost_since_us_ = 0;
    else if (vehicle_lost_since_us_ == 0) vehicle_lost_since_us_ = t;
    const bool lost = !v.vehicle_tracked && (t - vehicle_lost_since_us_) >= ms_to_us(vp.ignore_ms);

    switch (vphase_) {
    case VehiclePhase::Normal:
        if (v.vehicle_tracked && t >= vehicle_ignore_until_us_) enter(VehiclePhase::Follow);
        break;

    case VehiclePhase::Follow: {
        if ((!v.vehicle_tracked && !near) || lost) {
            vphase_ = VehiclePhase::Normal;
            parked_timing_ = false;
            break;
        }
        // Parked?  Stopped behind it (the gap cap brought the drive to 0), the camera
        // still sees it as a vehicle, and the distance does not grow.
        const bool stopped = near && tof_.drive() <= 0.0 && tof_driving();
        if (!stopped || busy || !v.vehicle_tracked) {
            parked_timing_ = false;
        } else if (!parked_timing_) {
            parked_timing_ = true;
            parked_since_us_ = t;
            parked_ref_c_ = v.C;
        } else if (v.C > parked_ref_c_ + vp.still_delta_mm) {
            parked_timing_ = false;   // it is moving off
        } else {
            if (v.C < parked_ref_c_) parked_ref_c_ = v.C;
            if (t - parked_since_us_ >= ms_to_us(vp.parked_confirm_ms)) {
                overtake_dir_ = choose_overtake_side(v.L, v.R);
                if (overtake_dir_ != RUMI_STEER_CENTER) {
                    enter(VehiclePhase::Out);
                    pass_seen_ = false;
                    pass_clear_timing_ = false;
                } else {
                    enter(VehiclePhase::Wait);   // parked_ref_c_ stays: the distance to watch
                }
            }
        }
        break;
    }

    case VehiclePhase::Wait:
        if ((!v.vehicle_tracked && !near) || lost) {
            vphase_ = VehiclePhase::Normal;
        } else if (v.C > parked_ref_c_ + vp.still_delta_mm) {
            enter(VehiclePhase::Follow);   // it moved off
        } else if (!busy && v.vehicle_tracked) {
            if (v.C < parked_ref_c_) parked_ref_c_ = v.C;
            overtake_dir_ = choose_overtake_side(v.L, v.R);
            if (overtake_dir_ != RUMI_STEER_CENTER) {
                enter(VehiclePhase::Out);
                pass_seen_ = false;
                pass_clear_timing_ = false;
            }
        }
        break;

    case VehiclePhase::Out:
    case VehiclePhase::Pass:
    case VehiclePhase::In: {
        if (busy || !tof_driving()) {   // a red light or an intersection, or the ToF logic took over: cut it short
            leave();
            break;
        }
        const int inner = overtake_dir_ == RUMI_STEER_LEFT ? v.R : v.L;   // the side the vehicle is on
        if (inner < vp.pass_clear_mm) pass_seen_ = true;
        const uint64_t elapsed = t - vphase_since_us_;
        if (vphase_ == VehiclePhase::Out) {
            if (elapsed >= ms_to_us(vp.out_ms)) {
                enter(VehiclePhase::Pass);
                pass_clear_timing_ = false;
            }
        } else if (vphase_ == VehiclePhase::Pass) {
            const bool clear = pass_seen_ && inner >= vp.pass_clear_mm;
            if (!clear) {
                pass_clear_timing_ = false;
            } else if (!pass_clear_timing_) {
                pass_clear_timing_ = true;
                pass_clear_since_us_ = t;
            }
            const bool passed = clear && elapsed >= ms_to_us(vp.pass_min_ms) && (t - pass_clear_since_us_) >= ms_to_us(vp.pass_clear_ms);
            if (passed || elapsed >= ms_to_us(vp.pass_max_ms)) enter(VehiclePhase::In);
        } else {
            if (elapsed >= ms_to_us(vp.in_ms)) leave();
        }
        break;
    }
    }
}

// ---------------------------------------------------------------------------
// One frame
// ---------------------------------------------------------------------------
int CamLogic::step(const rumi_sensor_frame& frame, rumi_command& out) {
    if (frame.size < sizeof(rumi_sensor_frame)) return 2;
    const uint64_t t = frame.t_us;
    if (first_) {
        first_ = false;
        sphase_since_us_ = vphase_since_us_ = iphase_since_us_ = t;
        vehicle_lost_since_us_ = t;
    }

    // --- perception: only when a new image accompanies this frame ----------------
    if (evaluator_ && frame.image) {
        const auto& pack = evaluator_->pack();
        if (frame.image_width != pack.image_w || frame.image_height != pack.image_h) return 3;
        vision::extract_features(frame.image, frame.image_width, frame.image_height, frame.image_stride, pack.config, features_);
        evaluator_->step(features_);
    }
    const View v = look();

    // --- behaviours ---------------------------------------------------------------
    step_signal(v, t);
    step_intersection(v, t);
    step_vehicle(v, t);

    // Speed: the lowest cap of the behaviours.
    int cap = RUMI_DRIVE_MAX;
    if (sphase_ == SignalPhase::Approach) cap = std::min(cap, p_.signal.approach_v);
    else if (sphase_ == SignalPhase::Stop) cap = 0;
    if (p_.intersection) {
        const IntersectionParams& ip = *p_.intersection;
        switch (iphase_) {
        case IntersectionPhase::Normal: break;
        case IntersectionPhase::Approach: cap = std::min(cap, ip.approach_v); break;
        case IntersectionPhase::Stop: cap = 0; break;
        case IntersectionPhase::Cross:
        case IntersectionPhase::Turn: cap = std::min(cap, ip.turn_v); break;
        }
    }
    if (p_.vehicle) {
        const VehicleParams& vp = *p_.vehicle;
        switch (vphase_) {
        case VehiclePhase::Normal: break;
        case VehiclePhase::Follow: cap = std::min(cap, follow_cap(v.C)); break;
        case VehiclePhase::Wait: cap = 0; break;
        case VehiclePhase::Out:
        case VehiclePhase::Pass:
        case VehiclePhase::In: cap = std::min(cap, vp.overtake_v); break;
        }
    }
    tof_.set_speed_cap(cap);

    // Steering: a manoeuvre forces it, following and crossing hold the lane.
    using SteerMode = tof::TofDriveLogic::SteerMode;
    if (vphase_ == VehiclePhase::Out) tof_.set_steer_mode(SteerMode::Force, overtake_dir_);
    else if (vphase_ == VehiclePhase::In) tof_.set_steer_mode(SteerMode::Force, opposite(overtake_dir_));
    else if (iphase_ == IntersectionPhase::Turn) tof_.set_steer_mode(SteerMode::Force, turn_dir_);
    else if (vphase_ == VehiclePhase::Pass || vphase_ == VehiclePhase::Wait || iphase_ == IntersectionPhase::Cross ||
             (vphase_ == VehiclePhase::Follow && v.C < tof::TofDriveLogic::OPEN_MM))
        tof_.set_steer_mode(SteerMode::HoldLane);
    else tof_.set_steer_mode(SteerMode::Auto);

    // --- the ToF logic drives, within the cap ----------------------------------------
    if (!tof_.step(frame, out)) return 2;
    const bool driving = tof_is_driving_state(out.state);
    if (cap == 0 && out.drive_permille == 0 && driving) out.flags |= RUMI_CMD_FLAG_BRAKE;   // hold still where a behaviour stops the car

    // The reported state: what explains the car's speed and steering best.  A
    // red light first; the signal's resume phase (red ignored, nothing else)
    // last, after any intersection or vehicle behaviour under way.
    if (driving) {
        if (sphase_ == SignalPhase::Approach || sphase_ == SignalPhase::Stop) {
            out.state = static_cast<uint8_t>(sphase_ == SignalPhase::Approach ? State::SignalApproach : State::SignalStop);
        } else if (iphase_ != IntersectionPhase::Normal) {
            out.state = static_cast<uint8_t>(iphase_ == IntersectionPhase::Approach ? State::IntersectionApproach
                                             : iphase_ == IntersectionPhase::Stop   ? State::IntersectionStop
                                             : iphase_ == IntersectionPhase::Cross  ? State::IntersectionCross
                                                                                    : State::IntersectionTurn);
        } else if (vphase_ != VehiclePhase::Normal) {
            out.state = static_cast<uint8_t>(vphase_ == VehiclePhase::Follow ? State::Follow
                                             : vphase_ == VehiclePhase::Wait ? State::FollowWait
                                             : vphase_ == VehiclePhase::Out  ? State::OvertakeOut
                                             : vphase_ == VehiclePhase::Pass ? State::OvertakePass
                                                                             : State::OvertakeIn);
        } else if (sphase_ == SignalPhase::Resume) {
            out.state = static_cast<uint8_t>(State::SignalResume);
        }
    }
    state_ = out.state;
    fill_aux(out);
    return 0;
}

} // namespace rumicar::cam
