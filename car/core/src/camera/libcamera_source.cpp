// libcamera-based image source (Raspberry Pi OS, Camera Module 2 on the
// vc4 ISP).  Frames are captured as packed RGB24 at the configured size,
// JPEG-encoded on libcamera's completion thread, and published as the
// newest image; the control loop only ever picks up the latest pointer.
#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <stdexcept>
#include <sys/mman.h>
#include <vector>

#include <libcamera/libcamera.h>

#include "../log.hpp"
#include "image_source.hpp"
#include "jpeg.hpp"

namespace rumicar::cam {

namespace {

using namespace libcamera;

class LibcameraSource final : public ImageSource {
public:
    explicit LibcameraSource(const CameraConfig& cfg) : cfg_(cfg) {}
    ~LibcameraSource() override { stop(); }

    void start() override {
        manager_ = std::make_unique<CameraManager>();
        if (manager_->start() != 0) throw std::runtime_error("libcamera: CameraManager::start failed");
        const auto cameras = manager_->cameras();
        if (cameras.empty()) throw std::runtime_error("libcamera: no camera found");
        camera_ = manager_->get(cameras[0]->id());
        if (!camera_) throw std::runtime_error("libcamera: cannot get camera " + cameras[0]->id());
        if (camera_->acquire() != 0) throw std::runtime_error("libcamera: camera busy (acquire failed)");
        id_ = camera_->id();

        config_ = camera_->generateConfiguration({StreamRole::Viewfinder});
        if (!config_ || config_->empty()) throw std::runtime_error("libcamera: no stream configuration for Viewfinder");
        StreamConfiguration& sc = config_->at(0);
        sc.size = Size(cfg_.width, cfg_.height);
        // DRM "BG24" = [23:0] B:G:R little endian, i.e. bytes R, G, B in memory: plain RGB24.
        sc.pixelFormat = formats::BGR888;
        sc.bufferCount = 4;
        const CameraConfiguration::Status st = config_->validate();
        if (st == CameraConfiguration::Invalid) throw std::runtime_error("libcamera: configuration rejected");
        if (sc.pixelFormat != formats::BGR888)
            throw std::runtime_error("libcamera: RGB24 (BGR888) not available, got " + sc.pixelFormat.toString());
        if (sc.size.width != cfg_.width || sc.size.height != cfg_.height)
            log::warn("libcamera: size adjusted to " + sc.size.toString());
        if (camera_->configure(config_.get()) != 0) throw std::runtime_error("libcamera: configure failed");
        stream_ = sc.stream();
        width_ = sc.size.width;
        height_ = sc.size.height;
        stride_ = sc.stride;

        allocator_ = std::make_unique<FrameBufferAllocator>(camera_);
        if (allocator_->allocate(stream_) < 0) throw std::runtime_error("libcamera: buffer allocation failed");
        for (const auto& buffer : allocator_->buffers(stream_)) {
            const FrameBuffer::Plane& plane = buffer->planes()[0];
            void* mem = ::mmap(nullptr, plane.length, PROT_READ, MAP_SHARED, plane.fd.get(), static_cast<off_t>(plane.offset));
            if (mem == MAP_FAILED) throw std::runtime_error("libcamera: mmap of a frame buffer failed");
            mapped_[buffer.get()] = Mapped{static_cast<const uint8_t*>(mem), plane.length};
            std::unique_ptr<Request> request = camera_->createRequest();
            if (!request) throw std::runtime_error("libcamera: createRequest failed");
            if (request->addBuffer(stream_, buffer.get()) != 0) throw std::runtime_error("libcamera: addBuffer failed");
            requests_.push_back(std::move(request));
        }

        camera_->requestCompleted.connect(this, &LibcameraSource::on_completed);
        ControlList controls(controls::controls);
        const int64_t frame_us = 1000000 / (cfg_.fps > 0 ? cfg_.fps : 1);
        controls.set(controls::FrameDurationLimits, {frame_us, frame_us});
        if (camera_->start(&controls) != 0) throw std::runtime_error("libcamera: start failed");
        running_ = true;
        for (auto& r : requests_) camera_->queueRequest(r.get());
        log::info("camera " + id_ + " started: " + std::to_string(width_) + "x" + std::to_string(height_) + " RGB24, " +
                  std::to_string(cfg_.fps) + " fps, jpeg quality " + std::to_string(cfg_.jpeg_quality));
    }

    void stop() override {
        if (camera_) {
            if (running_) {
                camera_->stop();
                running_ = false;
            }
            camera_->requestCompleted.disconnect(this);
            requests_.clear();
            for (auto& [buffer, m] : mapped_) ::munmap(const_cast<uint8_t*>(m.data), m.length);
            mapped_.clear();
            allocator_.reset();
            camera_->release();
            camera_.reset();
        }
        if (manager_) {
            manager_->stop();
            manager_.reset();
        }
    }

    std::shared_ptr<const Image> latest() const override {
        std::lock_guard<std::mutex> lock(latest_m_);
        return latest_;
    }

    uint64_t captured() const override { return captured_.load(); }

    std::string description() const override { return "libcamera " + id_; }

private:
    struct Mapped {
        const uint8_t* data;
        size_t length;
    };

    // libcamera's thread.
    void on_completed(Request* request) {
        if (request->status() == Request::RequestCancelled) return;
        if (request->status() == Request::RequestComplete) {
            const auto it = request->buffers().find(stream_);
            if (it != request->buffers().end()) {
                const FrameBuffer* buffer = it->second;
                const auto mit = mapped_.find(buffer);
                if (mit != mapped_.end() && buffer->metadata().status == FrameMetadata::FrameSuccess) {
                    try {
                        publish(mit->second, request->metadata());
                    } catch (const std::exception& e) {
                        if (encode_failures_++ % 100 == 0) log::error(std::string("camera frame dropped: ") + e.what());
                    }
                }
            }
        }
        request->reuse(Request::ReuseBuffers);
        camera_->queueRequest(request);
    }

    void publish(const Mapped& m, const ControlList& metadata) {
        auto img = std::make_shared<Image>();
        img->width = width_;
        img->height = height_;
        img->stride = width_ * 3;
        img->rgb.resize(static_cast<size_t>(img->stride) * height_);
        for (uint32_t y = 0; y < height_; ++y) {
            std::memcpy(img->rgb.data() + static_cast<size_t>(y) * img->stride, m.data + static_cast<size_t>(y) * stride_, img->stride);
        }
        img->jpeg = encoder_.encode(img->rgb.data(), width_, height_, img->stride, cfg_.jpeg_quality);
        if (const auto ts = metadata.get(controls::SensorTimestamp)) img->sensor_t_us = static_cast<uint64_t>(*ts / 1000);
        img->sequence = static_cast<uint32_t>(captured_.fetch_add(1) + 1);
        std::lock_guard<std::mutex> lock(latest_m_);
        latest_ = std::move(img);
    }

    CameraConfig cfg_;
    std::unique_ptr<CameraManager> manager_;
    std::shared_ptr<Camera> camera_;
    std::unique_ptr<CameraConfiguration> config_;
    std::unique_ptr<FrameBufferAllocator> allocator_;
    std::vector<std::unique_ptr<Request>> requests_;
    std::map<const FrameBuffer*, Mapped> mapped_;
    Stream* stream_ = nullptr;
    std::string id_;
    uint32_t width_ = 0, height_ = 0, stride_ = 0;
    bool running_ = false;
    JpegEncoder encoder_;
    std::atomic<uint64_t> captured_{0};
    uint64_t encode_failures_ = 0;
    mutable std::mutex latest_m_;
    std::shared_ptr<const Image> latest_;
};

} // namespace

std::unique_ptr<ImageSource> make_image_source(const CameraConfig& cfg) {
    return std::make_unique<LibcameraSource>(cfg);
}

} // namespace rumicar::cam
