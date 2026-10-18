// Telemetry server (TCP 5601): streams encoded frames to every connected
// client.  publish() never blocks the control loop: each client has a bounded
// queue and its own sender thread; when a client cannot keep up, its oldest
// queued frames are dropped (the PC's recorder counts the sequence gaps).
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "socket.hpp"

namespace rumicar::net {

class TelemetryServer {
public:
    using Frame = std::shared_ptr<const std::vector<uint8_t>>;

    // queue_frames: frames kept per client before the oldest is dropped.
    TelemetryServer(std::string bind_address, uint16_t port, size_t queue_frames);
    ~TelemetryServer();
    TelemetryServer(const TelemetryServer&) = delete;
    TelemetryServer& operator=(const TelemetryServer&) = delete;

    void start();   // throws std::runtime_error when the port cannot be bound
    void stop();

    uint16_t port() const { return port_; }
    size_t client_count() const;
    uint64_t dropped_frames() const { return dropped_.load(); }

    void publish(Frame frame);

private:
    struct Client {
        Fd fd;
        std::string peer;
        std::deque<Frame> queue;
        std::mutex m;
        std::condition_variable cv;
        bool closing = false;
        std::thread thread;
    };

    void accept_loop();
    void client_loop(Client& c);
    void reap_locked();

    std::string bind_address_;
    uint16_t port_;
    size_t queue_frames_;
    Fd listen_;
    std::thread acceptor_;
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> dropped_{0};
    mutable std::mutex clients_m_;
    std::list<std::unique_ptr<Client>> clients_;
};

} // namespace rumicar::net
