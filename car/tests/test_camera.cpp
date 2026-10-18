// JPEG encoding used for telemetry images, and the camera configuration keys.
#include <cstdio>
#include <string>
#include <vector>

#include "camera/image_source.hpp"
#include "camera/jpeg.hpp"
#include "check.hpp"
#include "config.hpp"

using namespace rumicar;

namespace {

std::vector<uint8_t> gradient(uint32_t w, uint32_t h, uint32_t stride) {
    std::vector<uint8_t> rgb(static_cast<size_t>(stride) * h, 0);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            uint8_t* p = rgb.data() + static_cast<size_t>(y) * stride + static_cast<size_t>(x) * 3;
            p[0] = static_cast<uint8_t>(x * 255 / (w - 1));
            p[1] = static_cast<uint8_t>(y * 255 / (h - 1));
            p[2] = static_cast<uint8_t>((x + y) & 0xFF);
        }
    }
    return rgb;
}

void test_jpeg_round_trip() {
    cam::JpegEncoder enc;
    const auto rgb = gradient(320, 240, 320 * 3);
    const auto jpeg = enc.encode(rgb.data(), 320, 240, 320 * 3, 80);
    CHECK(jpeg.size() > 1000);
    CHECK(jpeg.size() < rgb.size() / 4);
    CHECK(jpeg[0] == 0xFF && jpeg[1] == 0xD8);                       // SOI
    CHECK(jpeg[jpeg.size() - 2] == 0xFF && jpeg[jpeg.size() - 1] == 0xD9); // EOI
    uint32_t w = 0, h = 0;
    cam::jpeg_size(jpeg, w, h);
    CHECK(w == 320 && h == 240);

    // Rows with padding (stride > width * 3) are handled.
    const auto padded = gradient(100, 50, 100 * 3 + 64);
    const auto jpeg2 = enc.encode(padded.data(), 100, 50, 100 * 3 + 64, 60);
    cam::jpeg_size(jpeg2, w, h);
    CHECK(w == 100 && h == 50);

    // Lower quality -> smaller file.
    const auto small = enc.encode(rgb.data(), 320, 240, 320 * 3, 20);
    CHECK(small.size() < jpeg.size());
}

void test_jpeg_rejects_bad_input() {
    cam::JpegEncoder enc;
    const auto rgb = gradient(16, 16, 48);
    bool threw = false;
    try { enc.encode(rgb.data(), 16, 16, 48, 0); } catch (const std::exception&) { threw = true; }
    CHECK(threw);
    threw = false;
    try { enc.encode(rgb.data(), 16, 16, 40, 50); } catch (const std::exception&) { threw = true; }   // stride too small
    CHECK(threw);
    threw = false;
    uint32_t w, h;
    try { cam::jpeg_size(std::vector<uint8_t>{1, 2, 3}, w, h); } catch (const std::exception&) { threw = true; }
    CHECK(threw);
}

void test_camera_config() {
    const CoreConfig d = parse_core_config("{}");
    CHECK(d.camera_enabled && d.camera_width == 320 && d.camera_height == 240 && d.camera_fps == 20 && d.camera_jpeg_quality == 80);
    const CoreConfig c = parse_core_config(R"({"camera": {"enabled": false, "width": 640, "height": 480, "fps": 10, "jpeg_quality": 50}})");
    CHECK(!c.camera_enabled && c.camera_width == 640 && c.camera_height == 480 && c.camera_fps == 10 && c.camera_jpeg_quality == 50);
    for (const char* bad : {
             R"({"camera": {"enabled": "yes"}})",
             R"({"camera": {"width": 321}})",
             R"({"camera": {"fps": 0}})",
             R"({"camera": {"jpeg_quality": 101}})",
             R"({"camera": {"resolution": "320x240"}})",
         }) {
        bool threw = false;
        try { parse_core_config(bad); } catch (const std::exception&) { threw = true; }
        CHECK_MSG(threw, bad);
    }
}

} // namespace

int main() {
    test_jpeg_round_trip();
    test_jpeg_rejects_bad_input();
    test_camera_config();
    return check::summary("test_camera");
}
