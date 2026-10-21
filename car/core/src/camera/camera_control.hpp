// Runs the camera only while something needs images (docs 3.9): the control
// loop states what it wants each frame (set_wanted / set_jpeg) and a worker
// thread starts or stops the image source accordingly, so the loop never
// waits for libcamera.  A failed start is retried at retry_interval while
// images are still wanted.
#pragma once

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "image_source.hpp"

namespace rumicar::cam {

class CameraController {
public:
    CameraController(std::unique_ptr<ImageSource> source, std::chrono::milliseconds retry_interval);
    ~CameraController();
    CameraController(const CameraController&) = delete;
    CameraController& operator=(const CameraController&) = delete;

    // Desired state; both are cheap and never block on the camera.
    void set_wanted(bool wanted);
    void set_jpeg(bool enabled);

    // True while the camera is capturing.
    bool running() const;
    // Newest image while running, otherwise nullptr.
    std::shared_ptr<const Image> latest() const;
    // Number of start attempts that failed (for logs/tests).
    unsigned start_failures() const;

    // Stops the camera and the worker; called by the destructor as well.
    void shutdown();

    std::string description() const { return source_->description(); }

private:
    void worker();

    std::unique_ptr<ImageSource> source_;
    const std::chrono::milliseconds retry_interval_;
    mutable std::mutex m_;
    std::condition_variable cv_;
    bool wanted_ = false;
    bool jpeg_ = false;
    bool running_ = false;
    bool quit_ = false;
    unsigned failures_ = 0;
    std::chrono::steady_clock::time_point next_attempt_{};
    std::thread thread_;
};

} // namespace rumicar::cam
