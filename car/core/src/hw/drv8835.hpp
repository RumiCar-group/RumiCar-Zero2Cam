// DRV8835 dual H-bridge in IN/IN mode, wired as on the RumiCar chassis:
//   A channel = steering (AIN1/AIN2 on GPIO, software PWM: RC_steer's strength)
//   B channel = drive    (BIN1/BIN2 on hardware PWM, slow decay)
// Behaviour is that of RumiCar-lib v0.6.0 (RC_steer / RC_drive /
// driveSlowDecay), and so are the values: steering LEFT/CENTER/RIGHT with a
// strength 0..255, drive FREE/REVERSE/FORWARD/BRAKE with a PWM duty 0..255
// (the share of full duty).
//
// Start kick (as RC_drive's KICK_DUTY): when the drive starts from rest or
// reverses, the duty is raised to the kick duty for kick.duration_ms to
// overcome static friction.  The kick never exceeds the cap the caller gives
// for the frame (the limit of the driving mode, docs 3.4), and it ends after
// kick.duration_ms on its own: a worker thread lowers the duty at that time,
// independent of how often apply() is called (the control loop runs every
// 50 ms, longer than the kick).
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include "pwm.hpp"
#include "soft_pwm.hpp"

namespace rumicar::hw {

class Drv8835 {
public:
    struct Pins {
        unsigned drive_in1;   // BIN1 pwm channel
        unsigned drive_in2;   // BIN2 pwm channel
    };
    struct Kick {
        int pwm;              // duty of the start kick (0..255)
        int duration_ms;
    };

    // steer_write(index, level) drives AIN1 (index 0) and AIN2 (index 1);
    // steer_frequency_hz: the software PWM of the steering inputs (50..2000).
    // pwm_chip: the sysfs PWM chip whose channels drive_in1 / drive_in2 are BIN1 / BIN2.
    Drv8835(SoftPwmPair::Write steer_write, int steer_frequency_hz, const std::string& pwm_chip, int pwm_frequency_hz,
            Pins pins, Kick kick);
    ~Drv8835();
    Drv8835(const Drv8835&) = delete;
    Drv8835& operator=(const Drv8835&) = delete;

    // steer: RUMI_STEER_LEFT / CENTER / RIGHT (0 / 1 / 2), steer_pwm: its
    // strength 0..255 (ignored for CENTER; 0 = neutral, 255 = full).
    // drive_dir: RUMI_DRIVE_FREE / REVERSE / FORWARD / BRAKE (0 / 1 / 2 / 3),
    // drive_pwm: 0..255 (ignored for FREE and BRAKE).
    // kick_cap: the highest duty a start kick may use in this frame (0..255:
    // the limit of the driving mode); the kick is min(kick.pwm, kick_cap).
    // Thread-safe.
    void apply(uint8_t steer, uint8_t steer_pwm, uint8_t drive_dir, uint8_t drive_pwm, int kick_cap);

    // The duties of AIN1 and AIN2 (0..255) for a steering command, as RC_steer
    // of RumiCar-lib v0.6.0 writes them: CENTER, or a strength of 0, leaves
    // both low (neutral); RIGHT holds AIN1 high and puts 255 - s on AIN2 (slow
    // decay; s = 255 leaves AIN2 low: full steering); LEFT the other way round.
    // An unknown direction is neutral.
    static std::pair<int, int> steer_inputs(uint8_t steer, uint8_t steer_pwm);

    // Brake the drive motor and centre the steering (ends a kick).  Thread-safe.
    void stop();

    // Whether the kick worker runs with SCHED_FIFO (false until the first kick).
    bool kick_realtime() const;

private:
    using Clock = std::chrono::steady_clock;
    enum class Dir { Coast, Forward, Reverse, Brake };
    void set_drive_locked(Dir dir, int pwm);
    void kick_worker();

    Pins pins_;
    Kick kick_;
    SoftPwmPair steer_;    // AIN1 (line 0), AIN2 (line 1)
    std::unique_ptr<PwmChannel> in1_;
    std::unique_ptr<PwmChannel> in2_;
    mutable std::mutex mutex_;
    std::condition_variable kick_wake_;
    Dir last_dir_ = Dir::Coast;
    int last_pwm_ = 0;            // the duty commanded last (without the kick)
    bool kicking_ = false;
    Clock::time_point kick_until_{};
    uint64_t kick_generation_ = 0;   // incremented whenever a kick starts or is ended early
    bool kick_realtime_ = false;
    bool quit_ = false;
    std::thread kick_thread_;
};

} // namespace rumicar::hw
