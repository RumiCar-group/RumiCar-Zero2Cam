// ST VL53L0X time-of-flight sensor on I2C, continuous back-to-back ranging.
// Initialisation sequence ported from RumiCar-group/RumiCarRos rumi_hw (MIT).
#pragma once

#include <cstdint>
#include <string>

#include "gpio.hpp"
#include "i2c.hpp"

namespace rumicar::hw {

class Vl53l0x {
public:
    enum class Status { Ok, NoTarget, Timeout, Error };
    struct Reading {
        Status status = Status::Error;
        int mm = -1;
    };

    // `xshut` must be one of the lines requested in `gpio`; it is driven low
    // (sensor held in reset) until init() is called.
    Vl53l0x(I2cBus& bus, GpioOutputs& gpio, unsigned xshut, uint8_t address, std::string label);
    ~Vl53l0x();
    Vl53l0x(const Vl53l0x&) = delete;
    Vl53l0x& operator=(const Vl53l0x&) = delete;

    // Releases XSHUT, moves the sensor from the default address 0x29 to its own
    // address, loads the tuning settings and starts continuous ranging.
    // Other sensors on the bus must still be held in reset (or already moved
    // off 0x29) when this is called.  Throws std::runtime_error.
    void init();

    // Reads the latest measurement, waiting up to timeout_ms for one.
    Reading read(int timeout_ms);

    const std::string& label() const { return label_; }

private:
    void update_address();
    void store_stop_variable();
    void static_init();
    void start_continuous();

    I2cBus& bus_;
    GpioOutputs& gpio_;
    unsigned xshut_;
    uint8_t address_;
    std::string label_;
    uint8_t stop_variable_ = 0;
};

} // namespace rumicar::hw
