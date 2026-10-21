#include "camera_control.hpp"

#include <stdexcept>

#include "../log.hpp"

namespace rumicar::cam {

using Clock = std::chrono::steady_clock;

CameraController::CameraController(std::unique_ptr<ImageSource> source, std::chrono::milliseconds retry_interval)
    : source_(std::move(source)), retry_interval_(retry_interval) {
    if (!source_) throw std::invalid_argument("CameraController: no image source");
    source_->set_jpeg_enabled(jpeg_);   // nobody receives frames yet
    thread_ = std::thread([this] { worker(); });
}

CameraController::~CameraController() { shutdown(); }

void CameraController::set_wanted(bool wanted) {
    {
        std::lock_guard<std::mutex> lock(m_);
        if (wanted_ == wanted) return;
        wanted_ = wanted;
    }
    cv_.notify_all();
}

void CameraController::set_jpeg(bool enabled) {
    {
        std::lock_guard<std::mutex> lock(m_);
        if (jpeg_ == enabled) return;
        jpeg_ = enabled;
    }
    source_->set_jpeg_enabled(enabled);
}

bool CameraController::running() const {
    std::lock_guard<std::mutex> lock(m_);
    return running_;
}

std::shared_ptr<const Image> CameraController::latest() const {
    {
        std::lock_guard<std::mutex> lock(m_);
        if (!running_) return nullptr;
    }
    return source_->latest();
}

unsigned CameraController::start_failures() const {
    std::lock_guard<std::mutex> lock(m_);
    return failures_;
}

void CameraController::shutdown() {
    {
        std::lock_guard<std::mutex> lock(m_);
        if (quit_) return;
        quit_ = true;
        wanted_ = false;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

// The only thread that calls start()/stop() on the source.
void CameraController::worker() {
    std::unique_lock<std::mutex> lock(m_);
    while (true) {
        // Wait until the desired state differs from the actual one (or until
        // the retry time for a failed start), or until shutdown.
        if (wanted_ == running_ && !quit_) {
            cv_.wait(lock, [&] { return quit_ || wanted_ != running_; });
        } else if (wanted_ && !running_ && next_attempt_ > Clock::now() && !quit_) {
            cv_.wait_until(lock, next_attempt_, [&] { return quit_ || !wanted_; });
        }
        if (quit_) break;
        if (wanted_ == running_) continue;
        if (wanted_ && Clock::now() < next_attempt_) continue;

        const bool want = wanted_;
        lock.unlock();
        if (want) {
            try {
                source_->start();
                lock.lock();
                running_ = true;
                failures_ = 0;
                log::info("camera started: " + source_->description());
            } catch (const std::exception& e) {
                lock.lock();
                ++failures_;
                next_attempt_ = Clock::now() + retry_interval_;
                if (failures_ == 1 || failures_ % 20 == 0)
                    log::error(std::string("camera not started (attempt ") + std::to_string(failures_) + "), retrying in " +
                               std::to_string(retry_interval_.count()) + " ms: " + e.what());
            }
        } else {
            try {
                source_->stop();
            } catch (const std::exception& e) {
                log::error(std::string("camera stop: ") + e.what());
            }
            lock.lock();
            running_ = false;
            log::info("camera stopped (not needed)");
        }
    }
    // Shutting down: make sure the camera is off.
    const bool was_running = running_;
    lock.unlock();
    if (was_running) {
        try {
            source_->stop();
        } catch (const std::exception& e) {
            log::error(std::string("camera stop: ") + e.what());
        }
    }
    lock.lock();
    running_ = false;
}

} // namespace rumicar::cam
