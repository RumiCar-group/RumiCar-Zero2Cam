// Image source used when the core is built without libcamera
// (-DRUMICAR_WITH_CAMERA=OFF, e.g. on a build host without the Raspberry Pi
// libcamera packages).  start() reports the situation; the loop drives on.
#include <stdexcept>

#include "image_source.hpp"

namespace rumicar::cam {

namespace {

class NoCameraSource final : public ImageSource {
public:
    void start() override { throw std::runtime_error("this build of rumicar-core has no camera support (RUMICAR_WITH_CAMERA=OFF)"); }
    void stop() override {}
    std::shared_ptr<const Image> latest() const override { return nullptr; }
    uint64_t captured() const override { return 0; }
    std::string description() const override { return "none"; }
};

} // namespace

std::unique_ptr<ImageSource> make_image_source(const CameraConfig&) {
    return std::make_unique<NoCameraSource>();
}

} // namespace rumicar::cam
