#include "vl53l0x.hpp"

#include <chrono>
#include <stdexcept>
#include <thread>

namespace rumicar::hw {

namespace {

using namespace std::chrono_literals;

enum Reg : uint8_t {
    SYSRANGE_START = 0x00,
    SYSTEM_SEQUENCE_CONFIG = 0x01,
    SYSTEM_INTERRUPT_CONFIG_GPIO = 0x0A,
    SYSTEM_INTERRUPT_CLEAR = 0x0B,
    RESULT_INTERRUPT_STATUS = 0x13,
    RESULT_RANGE_MM = 0x1E,
    FINAL_RANGE_CONFIG_MIN_COUNT_RATE_RTN_LIMIT = 0x44,
    MSRC_CONFIG_CONTROL = 0x60,
    VHV_CONFIG_PAD_SCL_SDA__EXTSUP_HV = 0x89,
    I2C_SLAVE_DEVICE_ADDRESS = 0x8A,
    STOP_VARIABLE = 0x91,
    IDENTIFICATION_MODEL_ID = 0xC0,
};

constexpr uint8_t kDefaultAddress = 0x29;
constexpr uint8_t kExpectedModelId = 0xEE;
constexpr int kNoTargetMm = 8190; // the sensor reports 8190/8191 when nothing reflects

} // namespace

Vl53l0x::Vl53l0x(I2cBus& bus, GpioOutputs& gpio, unsigned xshut, uint8_t address, std::string label)
    : bus_(bus), gpio_(gpio), xshut_(xshut), address_(address), label_(std::move(label)) {
    gpio_.set(xshut_, false);
}

Vl53l0x::~Vl53l0x() {
    try {
        gpio_.set(xshut_, false);
    } catch (...) {
    }
}

void Vl53l0x::init() {
    gpio_.set(xshut_, true);
    std::this_thread::sleep_for(10ms);
    update_address();
    store_stop_variable();
    static_init();
    start_continuous();
}

void Vl53l0x::update_address() {
    if (address_ == kDefaultAddress) throw std::runtime_error(label_ + ": address 0x29 is reserved for unconfigured sensors");
    if (bus_.read8(kDefaultAddress, IDENTIFICATION_MODEL_ID) != kExpectedModelId)
        throw std::runtime_error(label_ + ": sensor not found at the default address 0x29");
    // Enable 2.8 V I/O (bit 0 of VHV_CONFIG_PAD_SCL_SDA__EXTSUP_HV).
    bus_.write8(kDefaultAddress, VHV_CONFIG_PAD_SCL_SDA__EXTSUP_HV,
                static_cast<uint8_t>(bus_.read8(kDefaultAddress, VHV_CONFIG_PAD_SCL_SDA__EXTSUP_HV) | 0x01));
    bus_.write8(kDefaultAddress, I2C_SLAVE_DEVICE_ADDRESS, static_cast<uint8_t>(address_ & 0x7F));
    if (bus_.read8(address_, IDENTIFICATION_MODEL_ID) != kExpectedModelId)
        throw std::runtime_error(label_ + ": sensor did not answer at its new address");
}

void Vl53l0x::store_stop_variable() {
    bus_.write8(address_, 0x88, 0x00); // I2C standard mode
    static const uint8_t prep[] = {0x80, 0x01, 0xFF, 0x01, 0x00, 0x00};
    bus_.write_sequence(address_, prep);
    stop_variable_ = bus_.read8(address_, STOP_VARIABLE);
    static const uint8_t done[] = {0x00, 0x01, 0xFF, 0x00, 0x80, 0x00};
    bus_.write_sequence(address_, done);
}

void Vl53l0x::static_init() {
    bus_.write8(address_, VHV_CONFIG_PAD_SCL_SDA__EXTSUP_HV,
                static_cast<uint8_t>(bus_.read8(address_, VHV_CONFIG_PAD_SCL_SDA__EXTSUP_HV) | 0x01));
    bus_.write8(address_, MSRC_CONFIG_CONTROL, static_cast<uint8_t>(bus_.read8(address_, MSRC_CONFIG_CONTROL) | 0x12));
    bus_.write16(address_, FINAL_RANGE_CONFIG_MIN_COUNT_RATE_RTN_LIMIT, 0x40);
    bus_.write8(address_, SYSTEM_SEQUENCE_CONFIG, 0xFF);

    // Default tuning settings from the ST API.
    static const uint8_t tuning[] = {
        0xFF, 0x01, 0x00, 0x00, 0xFF, 0x00, 0x09, 0x00, 0x10, 0x00, 0x11, 0x00, 0x24, 0x01, 0x25, 0xFF,
        0x75, 0x00, 0xFF, 0x01, 0x4E, 0x2C, 0x48, 0x00, 0x30, 0x20, 0xFF, 0x00, 0x30, 0x09, 0x54, 0x00,
        0x31, 0x04, 0x32, 0x03, 0x40, 0x83, 0x46, 0x25, 0x60, 0x00, 0x27, 0x00, 0x50, 0x06, 0x51, 0x00,
        0x52, 0x96, 0x56, 0x08, 0x57, 0x30, 0x61, 0x00, 0x62, 0x00, 0x64, 0x00, 0x65, 0x00, 0x66, 0xA0,
        0xFF, 0x01, 0x22, 0x32, 0x47, 0x14, 0x49, 0xFF, 0x4A, 0x00, 0xFF, 0x00, 0x7A, 0x0A, 0x7B, 0x00,
        0x78, 0x21, 0xFF, 0x01, 0x23, 0x34, 0x42, 0x00, 0x44, 0xFF, 0x45, 0x26, 0x46, 0x05, 0x40, 0x40,
        0x0E, 0x06, 0x20, 0x1A, 0x43, 0x40, 0xFF, 0x00, 0x34, 0x03, 0x35, 0x44, 0xFF, 0x01, 0x31, 0x04,
        0x4B, 0x09, 0x4C, 0x05, 0x4D, 0x04, 0xFF, 0x00, 0x44, 0x00, 0x45, 0x20, 0x47, 0x08, 0x48, 0x28,
        0x67, 0x00, 0x70, 0x04, 0x71, 0x01, 0x72, 0xFE, 0x76, 0x00, 0x77, 0x00, 0xFF, 0x01, 0x0D, 0x01,
        0xFF, 0x00, 0x80, 0x01, 0x01, 0xF8, 0xFF, 0x01, 0x8E, 0x01, 0x00, 0x01, 0xFF, 0x00, 0x80, 0x00,
    };
    bus_.write_sequence(address_, tuning);

    bus_.write8(address_, SYSTEM_INTERRUPT_CONFIG_GPIO, 0x04); // interrupt on new sample ready
    bus_.write8(address_, SYSTEM_INTERRUPT_CLEAR, 0x01);
    bus_.write8(address_, SYSTEM_SEQUENCE_CONFIG, 0xE1);
}

void Vl53l0x::start_continuous() {
    static const uint8_t pre[] = {0x80, 0x01, 0xFF, 0x01, 0x00, 0x00};
    bus_.write_sequence(address_, pre);
    bus_.write8(address_, STOP_VARIABLE, stop_variable_);
    static const uint8_t post[] = {0x00, 0x01, 0xFF, 0x00, 0x80, 0x00};
    bus_.write_sequence(address_, post);
    bus_.write8(address_, SYSRANGE_START, 0x02); // back-to-back continuous mode
}

Vl53l0x::Reading Vl53l0x::read(int timeout_ms) {
    Reading r;
    try {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while ((bus_.read8(address_, RESULT_INTERRUPT_STATUS) & 0x07) == 0) {
            if (std::chrono::steady_clock::now() >= deadline) {
                r.status = Status::Timeout;
                return r;
            }
            std::this_thread::sleep_for(1ms);
        }
        const uint16_t range = bus_.read16(address_, RESULT_RANGE_MM);
        bus_.write8(address_, SYSTEM_INTERRUPT_CLEAR, 0x01);
        if (range >= kNoTargetMm) {
            r.status = Status::NoTarget;
            r.mm = kNoTargetMm;
        } else {
            r.status = Status::Ok;
            r.mm = range;
        }
    } catch (const std::exception&) {
        r.status = Status::Error;
        r.mm = -1;
    }
    return r;
}

} // namespace rumicar::hw
