#include "socket.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace rumicar::net {

namespace {
std::string errno_text(const char* what) { return std::string(what) + ": " + std::strerror(errno); }
}

Fd& Fd::operator=(Fd&& o) noexcept {
    if (this != &o) {
        close();
        fd_ = o.fd_;
        o.fd_ = -1;
    }
    return *this;
}

int Fd::release() {
    const int f = fd_;
    fd_ = -1;
    return f;
}

void Fd::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

void Fd::shutdown() {
    if (fd_ >= 0) ::shutdown(fd_, SHUT_RDWR);
}

Fd listen_tcp(const std::string& bind_address, uint16_t port, int backlog) {
    Fd fd(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    if (!fd.valid()) throw std::runtime_error(errno_text("socket"));
    const int one = 1;
    ::setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, bind_address.c_str(), &addr.sin_addr) != 1)
        throw std::runtime_error("bad bind address '" + bind_address + "'");
    if (::bind(fd.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0)
        throw std::runtime_error(errno_text(("bind " + bind_address + ":" + std::to_string(port)).c_str()));
    if (::listen(fd.get(), backlog) != 0) throw std::runtime_error(errno_text("listen"));
    return fd;
}

uint16_t bound_port(int listen_fd) {
    sockaddr_in addr{};
    socklen_t len = sizeof addr;
    if (::getsockname(listen_fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) throw std::runtime_error(errno_text("getsockname"));
    return ntohs(addr.sin_port);
}

Fd accept_client(int listen_fd, std::string& peer) {
    sockaddr_in addr{};
    socklen_t len = sizeof addr;
    while (true) {
        const int c = ::accept4(listen_fd, reinterpret_cast<sockaddr*>(&addr), &len, SOCK_CLOEXEC);
        if (c >= 0) {
            char text[INET_ADDRSTRLEN] = {0};
            ::inet_ntop(AF_INET, &addr.sin_addr, text, sizeof text);
            peer = std::string(text) + ":" + std::to_string(ntohs(addr.sin_port));
            return Fd(c);
        }
        if (errno == EINTR) continue;
        return Fd();   // EINVAL after shutdown(), EBADF after close(): stop accepting
    }
}

void set_timeouts(int fd, int send_ms, int recv_ms) {
    timeval snd{send_ms / 1000, (send_ms % 1000) * 1000};
    timeval rcv{recv_ms / 1000, (recv_ms % 1000) * 1000};
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &snd, sizeof snd);
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcv, sizeof rcv);
}

void set_nodelay(int fd) {
    const int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
}

void write_all(int fd, const void* data, size_t len) {
    const auto* p = static_cast<const uint8_t*>(data);
    size_t done = 0;
    while (done < len) {
        const ssize_t n = ::send(fd, p + done, len - done, MSG_NOSIGNAL);
        if (n > 0) {
            done += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) throw std::runtime_error("send timed out");
        throw std::runtime_error(errno_text("send"));
    }
}

long read_some(int fd, void* buf, size_t len) {
    while (true) {
        const ssize_t n = ::recv(fd, buf, len, 0);
        if (n >= 0) return static_cast<long>(n);
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return -1;
        throw std::runtime_error(errno_text("recv"));
    }
}

} // namespace rumicar::net
