// Control server (TCP 5600): one JSON object per line, request/response plus
// unsolicited events (docs 5.2).  One client at a time; a second connection
// is answered with an error line and closed.
//
// Requests are decoded here and passed to a handler supplied by main.cpp,
// which owns the car state.  The handler runs on the client thread, never on
// the control loop.
#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "socket.hpp"

namespace rumicar::net {

// Thrown by a request handler to answer ok=false with a message.
struct RequestError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

class ControlServer {
public:
    // Returns the "result" value (json null = no result).  Throws RequestError for ok=false.
    using Handler = std::function<nlohmann::json(const std::string& type, const nlohmann::json& params)>;
    using ClientCallback = std::function<void(bool connected, const std::string& peer)>;

    ControlServer(std::string bind_address, uint16_t port, Handler handler, ClientCallback on_client);
    ~ControlServer();
    ControlServer(const ControlServer&) = delete;
    ControlServer& operator=(const ControlServer&) = delete;

    void start();   // throws std::runtime_error when the port cannot be bound
    void stop();

    uint16_t port() const { return port_; }
    bool client_connected() const { return client_active_.load(); }

    // Queue an event for the connected client (dropped when nobody is connected).
    void push_event(const std::string& event, const nlohmann::json& data);

    static constexpr size_t kMaxLineBytes = 64 * 1024;
    static constexpr size_t kMaxQueuedEvents = 100;

private:
    void accept_loop();
    void client_loop(Fd fd, std::string peer);
    void flush_events(int fd);
    std::string handle_line(const std::string& line, bool& hello_done);

    std::string bind_address_;
    uint16_t port_;
    Handler handler_;
    ClientCallback on_client_;
    Fd listen_;
    std::thread acceptor_;
    std::thread client_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> client_active_{false};
    std::mutex client_fd_m_;
    int client_fd_ = -1;          // for shutdown() from stop()
    std::mutex events_m_;
    std::deque<std::string> events_;
};

} // namespace rumicar::net
