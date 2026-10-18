// GPIO output lines via libgpiod 2.x (Debian 13 / Raspberry Pi OS Trixie).
#pragma once

#include <string>
#include <vector>

struct gpiod_chip;
struct gpiod_line_request;

namespace rumicar::hw {

class GpioOutputs {
public:
    // Requests all `offsets` as outputs, initially inactive (low).
    // Throws std::runtime_error.
    GpioOutputs(const std::string& chip_path, const std::vector<unsigned>& offsets, const std::string& consumer);
    ~GpioOutputs();
    GpioOutputs(const GpioOutputs&) = delete;
    GpioOutputs& operator=(const GpioOutputs&) = delete;

    // Drive one requested line.  Throws std::runtime_error on failure.
    void set(unsigned offset, bool active);

private:
    gpiod_chip* chip_ = nullptr;
    gpiod_line_request* request_ = nullptr;
    std::vector<unsigned> offsets_;
};

} // namespace rumicar::hw
