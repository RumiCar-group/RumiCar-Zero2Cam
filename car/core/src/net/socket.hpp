// Small POSIX socket helpers shared by the control and telemetry servers.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace rumicar::net {

// An owning file descriptor (closed on destruction).
class Fd {
public:
    Fd() = default;
    explicit Fd(int fd) : fd_(fd) {}
    ~Fd() { close(); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& o) noexcept : fd_(o.fd_) { o.fd_ = -1; }
    Fd& operator=(Fd&& o) noexcept;

    int get() const { return fd_; }
    bool valid() const { return fd_ >= 0; }
    int release();
    void close();
    // shutdown(SHUT_RDWR): wakes up any thread blocked in read/accept on this socket.
    void shutdown();

private:
    int fd_ = -1;
};

// Listening TCP/IPv4 socket.  Throws std::runtime_error.  port 0 = ephemeral.
Fd listen_tcp(const std::string& bind_address, uint16_t port, int backlog);

// The port a listening socket is bound to.
uint16_t bound_port(int listen_fd);

// Blocking accept; returns an invalid Fd when the listening socket was shut down.
Fd accept_client(int listen_fd, std::string& peer);

// Send/receive timeouts (SO_SNDTIMEO / SO_RCVTIMEO) and TCP_NODELAY.
void set_timeouts(int fd, int send_ms, int recv_ms);
void set_nodelay(int fd);

// Writes everything or throws std::runtime_error (timeout, peer closed).
void write_all(int fd, const void* data, size_t len);

// Reads up to len bytes; 0 on orderly close; throws on error.  A receive
// timeout returns -1 so the caller can do periodic work and retry.
long read_some(int fd, void* buf, size_t len);

} // namespace rumicar::net
