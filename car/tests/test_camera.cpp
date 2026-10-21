// JPEG encoding used for telemetry images, the camera configuration keys, and
// the on-demand camera controller.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "camera/camera_control.hpp"
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

// A camera stand-in: start() fails `fail_starts` times, then "captures" one
// image per start; records the JPEG flag it was given.
class FakeSource final : public cam::ImageSource {
public:
    explicit FakeSource(int fail_starts) : fail_left_(fail_starts) {}
    void start() override {
        ++start_calls;
        if (fail_left_ > 0) { --fail_left_; throw std::runtime_error("no camera today"); }
        auto img = std::make_shared<cam::Image>();
        img->width = 4; img->height = 2; img->stride = 12;
        img->rgb.assign(24, 7);
        img->sequence = static_cast<uint32_t>(start_calls);
        std::lock_guard<std::mutex> lock(m_);
        latest_ = img;
        running_ = true;
    }
    void stop() override {
        ++stop_calls;
        std::lock_guard<std::mutex> lock(m_);
        latest_.reset();
        running_ = false;
    }
    std::shared_ptr<const cam::Image> latest() const override { std::lock_guard<std::mutex> lock(m_); return latest_; }
    void set_jpeg_enabled(bool enabled) override { jpeg = enabled; }
    uint64_t captured() const override { return 0; }
    std::string description() const override { return "fake"; }
    bool running() const { std::lock_guard<std::mutex> lock(m_); return running_; }

    std::atomic<int> start_calls{0};
    std::atomic<int> stop_calls{0};
    std::atomic<bool> jpeg{true};

private:
    int fail_left_;
    mutable std::mutex m_;
    std::shared_ptr<const cam::Image> latest_;
    bool running_ = false;
};

template <typename Pred>
bool wait_for(Pred pred, std::chrono::milliseconds limit = std::chrono::milliseconds(3000)) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (!pred()) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

void test_camera_controller_on_demand() {
    auto owned = std::make_unique<FakeSource>(0);
    FakeSource* fake = owned.get();
    cam::CameraController ctl(std::move(owned), std::chrono::milliseconds(50));
    // Nothing wanted: the camera stays off and there is no image.
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    CHECK(!ctl.running() && fake->start_calls.load() == 0 && ctl.latest() == nullptr);

    ctl.set_wanted(true);
    CHECK(wait_for([&] { return ctl.running(); }));
    CHECK(fake->start_calls.load() == 1);
    CHECK(ctl.latest() != nullptr && ctl.latest()->width == 4);

    // JPEG on/off reaches the source without touching the camera state.
    ctl.set_jpeg(false);
    CHECK(!fake->jpeg.load());
    ctl.set_jpeg(true);
    CHECK(fake->jpeg.load());
    ctl.set_jpeg(true);   // no change: still true
    CHECK(fake->jpeg.load() && ctl.running());

    ctl.set_wanted(false);
    CHECK(wait_for([&] { return !ctl.running(); }));
    CHECK(fake->stop_calls.load() == 1 && ctl.latest() == nullptr);

    // Wanting it again restarts it; shutdown stops it exactly once more.
    ctl.set_wanted(true);
    CHECK(wait_for([&] { return ctl.running(); }));
    CHECK(fake->start_calls.load() == 2);
    ctl.shutdown();
    CHECK(!ctl.running() && fake->stop_calls.load() == 2 && !fake->running());
    ctl.shutdown();   // idempotent
    CHECK(fake->stop_calls.load() == 2);
}

void test_camera_controller_retries_failed_start() {
    auto owned = std::make_unique<FakeSource>(2);   // the first two starts fail
    FakeSource* fake = owned.get();
    cam::CameraController ctl(std::move(owned), std::chrono::milliseconds(40));
    ctl.set_wanted(true);
    CHECK(wait_for([&] { return ctl.running(); }, std::chrono::milliseconds(5000)));
    CHECK(fake->start_calls.load() == 3);
    CHECK(ctl.start_failures() == 0);   // reset once a start succeeds
    ctl.set_wanted(false);
    CHECK(wait_for([&] { return !ctl.running(); }));

    // Giving up while a retry is pending stops the attempts.
    auto owned2 = std::make_unique<FakeSource>(1000);
    FakeSource* fake2 = owned2.get();
    cam::CameraController ctl2(std::move(owned2), std::chrono::milliseconds(20));
    ctl2.set_wanted(true);
    CHECK(wait_for([&] { return fake2->start_calls.load() >= 2; }));
    CHECK(ctl2.start_failures() >= 2 && !ctl2.running());
    ctl2.set_wanted(false);
    const int attempts = fake2->start_calls.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(fake2->start_calls.load() <= attempts + 1);   // at most one attempt already in flight
    CHECK(fake2->stop_calls.load() == 0);               // never started, so never stopped
}

int main() {
    test_jpeg_round_trip();
    test_jpeg_rejects_bad_input();
    test_camera_config();
    test_camera_controller_on_demand();
    test_camera_controller_retries_failed_start();
    return check::summary("test_camera");
}
