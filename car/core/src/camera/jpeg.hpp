// JPEG encoding of RGB24 images with libjpeg-turbo (TurboJPEG API).
#pragma once

#include <cstdint>
#include <vector>

namespace rumicar::cam {

class JpegEncoder {
public:
    JpegEncoder();
    ~JpegEncoder();
    JpegEncoder(const JpegEncoder&) = delete;
    JpegEncoder& operator=(const JpegEncoder&) = delete;

    // rgb: RGB24 rows of `stride` bytes.  quality 1..100.  Throws std::runtime_error.
    std::vector<uint8_t> encode(const uint8_t* rgb, uint32_t width, uint32_t height, uint32_t stride, int quality);

private:
    void* handle_ = nullptr;
};

// Width/height of a JPEG (header only).  Throws std::runtime_error on bad data.
void jpeg_size(const std::vector<uint8_t>& jpeg, uint32_t& width, uint32_t& height);

// Decodes a JPEG to tightly packed RGB24 (used by the replay tool, never on
// the car's control path).  Throws std::runtime_error on bad data.
struct DecodedImage {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgb;   // stride = width * 3
};
DecodedImage jpeg_decode(const std::vector<uint8_t>& jpeg);

} // namespace rumicar::cam
