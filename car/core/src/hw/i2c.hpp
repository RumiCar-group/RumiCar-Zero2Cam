// Linux I2C bus access (/dev/i2c-N) using I2C_RDWR transactions.
// Ported from RumiCar-group/RumiCarRos rumi_hw (MIT).
#pragma once

#include <cstdint>
#include <span>
#include <string>

namespace rumicar::hw {

class I2cBus {
public:
    explicit I2cBus(int bus);
    ~I2cBus();
    I2cBus(const I2cBus&) = delete;
    I2cBus& operator=(const I2cBus&) = delete;

    // All methods throw std::runtime_error on failure.
    void write(uint8_t address, uint8_t reg, std::span<const uint8_t> data);
    void read(uint8_t address, uint8_t reg, std::span<uint8_t> data);
    void write8(uint8_t address, uint8_t reg, uint8_t val);
    void write16(uint8_t address, uint8_t reg, uint16_t val);
    uint8_t read8(uint8_t address, uint8_t reg);
    uint16_t read16(uint8_t address, uint8_t reg);
    // Writes (register, value) pairs in order.
    void write_sequence(uint8_t address, std::span<const uint8_t> pairs);

private:
    int fd_ = -1;
    std::string label_;
};

} // namespace rumicar::hw
