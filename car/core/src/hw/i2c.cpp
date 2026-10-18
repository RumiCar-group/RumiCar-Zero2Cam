#include "i2c.hpp"

#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <stdexcept>

namespace rumicar::hw {

namespace {

std::string hex(uint8_t v) {
    static const char* d = "0123456789ABCDEF";
    return std::string("0x") + d[v >> 4] + d[v & 0x0F];
}

} // namespace

I2cBus::I2cBus(int bus) : label_("/dev/i2c-" + std::to_string(bus)) {
    fd_ = open(label_.c_str(), O_RDWR);
    if (fd_ < 0) throw std::runtime_error(label_ + ": open failed: " + std::strerror(errno));
}

I2cBus::~I2cBus() {
    if (fd_ >= 0) close(fd_);
}

void I2cBus::write(uint8_t address, uint8_t reg, std::span<const uint8_t> data) {
    std::array<uint8_t, 64> buffer{};
    if (data.size() > buffer.size() - 1) throw std::runtime_error(label_ + ": write too large");
    buffer[0] = reg;
    std::copy(data.begin(), data.end(), buffer.begin() + 1);

    i2c_msg msg{};
    msg.addr = address;
    msg.flags = 0;
    msg.len = static_cast<uint16_t>(data.size() + 1);
    msg.buf = buffer.data();
    i2c_rdwr_ioctl_data rdwr{};
    rdwr.msgs = &msg;
    rdwr.nmsgs = 1;
    if (ioctl(fd_, I2C_RDWR, &rdwr) < 0)
        throw std::runtime_error(label_ + ": write to " + hex(address) + " reg " + hex(reg) + " failed: " + std::strerror(errno));
}

void I2cBus::read(uint8_t address, uint8_t reg, std::span<uint8_t> data) {
    i2c_msg msgs[2]{};
    msgs[0].addr = address;
    msgs[0].flags = 0;
    msgs[0].len = 1;
    msgs[0].buf = &reg;
    msgs[1].addr = address;
    msgs[1].flags = I2C_M_RD;
    msgs[1].len = static_cast<uint16_t>(data.size());
    msgs[1].buf = data.data();
    i2c_rdwr_ioctl_data rdwr{};
    rdwr.msgs = msgs;
    rdwr.nmsgs = 2;
    if (ioctl(fd_, I2C_RDWR, &rdwr) < 0)
        throw std::runtime_error(label_ + ": read from " + hex(address) + " reg " + hex(reg) + " failed: " + std::strerror(errno));
}

void I2cBus::write8(uint8_t address, uint8_t reg, uint8_t val) { write(address, reg, {&val, 1}); }

void I2cBus::write16(uint8_t address, uint8_t reg, uint16_t val) {
    const uint8_t buffer[] = {static_cast<uint8_t>(val >> 8), static_cast<uint8_t>(val & 0xFF)};
    write(address, reg, buffer);
}

uint8_t I2cBus::read8(uint8_t address, uint8_t reg) {
    uint8_t val = 0;
    read(address, reg, {&val, 1});
    return val;
}

uint16_t I2cBus::read16(uint8_t address, uint8_t reg) {
    uint8_t buffer[2] = {0, 0};
    read(address, reg, buffer);
    return static_cast<uint16_t>((buffer[0] << 8) | buffer[1]);
}

void I2cBus::write_sequence(uint8_t address, std::span<const uint8_t> pairs) {
    if (pairs.size() % 2 != 0) throw std::runtime_error(label_ + ": write_sequence needs pairs");
    for (size_t i = 0; i < pairs.size(); i += 2) write8(address, pairs[i], pairs[i + 1]);
}

} // namespace rumicar::hw
