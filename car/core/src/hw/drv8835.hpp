// DRV8835 dual H-bridge in IN/IN mode, wired as on the RumiCar chassis:
//   A channel = steering (AIN1/AIN2 on GPIO, on/off: left / centre / right)
//   B channel = drive    (BIN1/BIN2 on hardware PWM, slow decay)
// Behaviour is that of RumiCar-lib (RC_steer / RC_drive / driveSlowDecay),
// and so are the values: steering LEFT/CENTER/RIGHT, drive FREE/REVERSE/
// FORWARD/BRAKE with a PWM duty 0..255 (the share of full duty).
#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>

#include "gpio.hpp"
#include "pwm.hpp"

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

    Drv8835(GpioOutputs& gpio, const std::string& pwm_chip, int pwm_frequency_hz, Pins pins, Kick kick);
    ~Drv8835();
    Drv8835(const Drv8835&) = delete;
    Drv8835& operator=(const Drv8835&) = delete;

    // steer: RUMI_STEER_LEFT / CENTER / RIGHT (0 / 1 / 2).
    // drive_dir: RUMI_DRIVE_FREE / REVERSE / FORWARD / BRAKE (0 / 1 / 2 / 3),
    // drive_pwm: 0..255 (ignored for FREE and BRAKE).
    // `now` drives the start-kick timing.  Thread-safe.
    void apply(uint8_t steer, uint8_t drive_dir, uint8_t drive_pwm, std::chrono::steady_clock::time_point now);

    // Brake the drive motor and centre the steering.  Thread-safe.
    void stop();

private:
    enum class Dir { Coast, Forward, Reverse, Brake };
    void set_drive_locked(Dir dir, int pwm);

    GpioOutputs& gpio_;
    Pins pins_;
    Kick kick_;
    std::unique_ptr<PwmChannel> in1_;
    std::unique_ptr<PwmChannel> in2_;
    std::mutex mutex_;
    Dir last_dir_ = Dir::Coast;
    std::chrono::steady_clock::time_point kick_until_{};
    bool kicking_ = false;
};

} // namespace rumicar::hw
