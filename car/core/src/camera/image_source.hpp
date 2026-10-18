// Camera abstraction used by the control loop.  The implementation is chosen
// at build time (CMake option RUMICAR_WITH_CAMERA): libcamera on the car, or a
// source that reports "no camera support" when the core is built without it.
// Either way the loop keeps driving when no image is available.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace rumicar::cam {

// One captured image, immutable once published.
struct Image {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;          // bytes per row of `rgb` (= width * 3, tightly packed)
    std::vector<uint8_t> rgb;     // RGB24, row-major
    std::vector<uint8_t> jpeg;    // the same image, JPEG encoded (for telemetry / recording)
    uint64_t sensor_t_us = 0;     // sensor timestamp (libcamera SensorTimestamp), microseconds
    uint32_t sequence = 0;        // increments per captured image
};

struct CameraConfig {
    unsigned width = 320;
    unsigned height = 240;
    int fps = 20;
    int jpeg_quality = 80;
};

class ImageSource {
public:
    virtual ~ImageSource() = default;

    // Opens the camera and begins capturing.  Throws std::runtime_error when
    // no usable camera is present (the caller then drives without images).
    virtual void start() = 0;
    virtual void stop() = 0;

    // Newest captured image, or nullptr before the first one.  Never blocks.
    virtual std::shared_ptr<const Image> latest() const = 0;

    virtual uint64_t captured() const = 0;
    virtual std::string description() const = 0;
};

// Factory for the implementation compiled in.
std::unique_ptr<ImageSource> make_image_source(const CameraConfig& cfg);

} // namespace rumicar::cam
