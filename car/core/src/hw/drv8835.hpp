// DRV8835 dual H-bridge in IN/IN mode, wired as on the RumiCar chassis:
//   A channel = steering (AIN1/AIN2 on GPIO, software PWM: RC_steer's strength)
//   B channel = drive    (BIN1/BIN2 on hardware PWM, slow decay)
// Behaviour is that of RumiCar-lib v0.6.0 (RC_steer / RC_drive /
// driveSlowDecay), and so are the values: steering LEFT/CENTER/RIGHT with a
// strength 0..255, drive FREE/REVERSE/FORWARD/BRAKE with a PWM duty 0..255
// (the share of full duty).
#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>

#include "gpio.hpp"
#include "pwm.hpp"
#include "soft_pwm.hpp"

namespace rumicar::hw {

class Drv8835 {
public:
    struct Pins {
        unsigned steer_in1;   // AIN1
        unsigned steer_in2;   // AIN2
        unsigned drive_in1;   // BIN1 pwm channel
        unsigned drive_in2;   // BIN2 pwm channel
    };
    struct Kick {
        int pwm;              // duty of the start kick (0..255)
        int duration_ms;
    };

    // steer_frequency_hz: the software PWM of the steering inputs (50..2000).
    Drv8835(GpioOutputs& gpio, int steer_frequency_hz, const std::string& pwm_chip, int pwm_frequency_hz, Pins pins, Kick kick);
    ~Drv8835();
    Drv8835(const Drv8835&) = delete;
    Drv8835& operator=(const Drv8835&) = delete;

    // steer: RUMI_STEER_LEFT / CENTER / RIGHT (0 / 1 / 2), steer_pwm: its
    // strength 0..255 (ignored for CENTER; 0 = neutral, 255 = full).
    // drive_dir: RUMI_DRIVE_FREE / REVERSE / FORWARD / BRAKE (0 / 1 / 2 / 3),
    // drive_pwm: 0..255 (ignored for FREE and BRAKE).
    // `now` drives the start-kick timing.  Thread-safe.
    void apply(uint8_t steer, uint8_t steer_pwm, uint8_t drive_dir, uint8_t drive_pwm,
               std::chrono::steady_clock::time_point now);

    // The duties of AIN1 and AIN2 (0..255) for a steering command, as RC_steer
    // of RumiCar-lib v0.6.0 writes them: CENTER, or a strength of 0, leaves
    // both low (neutral); RIGHT holds AIN1 high and puts 255 - s on AIN2 (slow
    // decay; s = 255 leaves AIN2 low: full steering); LEFT the other way round.
    // An unknown direction is neutral.
    static std::pair<int, int> steer_inputs(uint8_t steer, uint8_t steer_pwm);

    // Brake the drive motor and centre the steering.  Thread-safe.
    void stop();

private:
    enum class Dir { Coast, Forward, Reverse, Brake };
    void set_drive_locked(Dir dir, int pwm);

    Pins pins_;
    Kick kick_;
    SoftPwmPair steer_;    // AIN1 (line 0), AIN2 (line 1)
    std::unique_ptr<PwmChannel> in1_;
    std::unique_ptr<PwmChannel> in2_;
    std::mutex mutex_;
    Dir last_dir_ = Dir::Coast;
    std::chrono::steady_clock::time_point kick_until_{};
    bool kicking_ = false;
};

} // namespace rumicar::hw
