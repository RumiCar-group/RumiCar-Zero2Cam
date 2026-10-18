#include "gpio.hpp"

#include <gpiod.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <stdexcept>

namespace rumicar::hw {

namespace {

std::string err(const std::string& what) { return what + ": " + std::strerror(errno); }

} // namespace

GpioOutputs::GpioOutputs(const std::string& chip_path, const std::vector<unsigned>& offsets, const std::string& consumer)
    : offsets_(offsets) {
    if (offsets_.empty()) throw std::runtime_error("GpioOutputs: no offsets");

    chip_ = gpiod_chip_open(chip_path.c_str());
    if (!chip_) throw std::runtime_error(err("gpiod_chip_open(" + chip_path + ")"));

    gpiod_line_settings* settings = gpiod_line_settings_new();
    gpiod_line_config* line_cfg = gpiod_line_config_new();
    gpiod_request_config* req_cfg = gpiod_request_config_new();
    try {
        if (!settings || !line_cfg || !req_cfg) throw std::runtime_error(err("gpiod config allocation"));
        if (gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_OUTPUT) != 0)
            throw std::runtime_error(err("gpiod_line_settings_set_direction"));
        if (gpiod_line_settings_set_output_value(settings, GPIOD_LINE_VALUE_INACTIVE) != 0)
            throw std::runtime_error(err("gpiod_line_settings_set_output_value"));
        if (gpiod_line_config_add_line_settings(line_cfg, offsets_.data(), offsets_.size(), settings) != 0)
            throw std::runtime_error(err("gpiod_line_config_add_line_settings"));
        gpiod_request_config_set_consumer(req_cfg, consumer.c_str());
        request_ = gpiod_chip_request_lines(chip_, req_cfg, line_cfg);
        if (!request_) throw std::runtime_error(err("gpiod_chip_request_lines"));
    } catch (...) {
        if (req_cfg) gpiod_request_config_free(req_cfg);
        if (line_cfg) gpiod_line_config_free(line_cfg);
        if (settings) gpiod_line_settings_free(settings);
        gpiod_chip_close(chip_);
        chip_ = nullptr;
        throw;
    }
    gpiod_request_config_free(req_cfg);
    gpiod_line_config_free(line_cfg);
    gpiod_line_settings_free(settings);
}

GpioOutputs::~GpioOutputs() {
    if (request_) {
        // Leave every line inactive: motor driver inputs low = coast.
        for (unsigned off : offsets_) gpiod_line_request_set_value(request_, off, GPIOD_LINE_VALUE_INACTIVE);
        gpiod_line_request_release(request_);
    }
    if (chip_) gpiod_chip_close(chip_);
}

void GpioOutputs::set(unsigned offset, bool active) {
    if (std::find(offsets_.begin(), offsets_.end(), offset) == offsets_.end())
        throw std::runtime_error("GpioOutputs: offset " + std::to_string(offset) + " was not requested");
    if (gpiod_line_request_set_value(request_, offset, active ? GPIOD_LINE_VALUE_ACTIVE : GPIOD_LINE_VALUE_INACTIVE) != 0)
        throw std::runtime_error(err("gpiod_line_request_set_value(" + std::to_string(offset) + ")"));
}

} // namespace rumicar::hw
