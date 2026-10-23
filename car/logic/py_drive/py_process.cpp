#include "py_process.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

namespace rumicar::py {

namespace {

constexpr size_t kMaxLine = 4096;   // longer lines from the runner are cut (they are never protocol lines)

std::string errno_text(const char* what) { return std::string(what) + ": " + std::strerror(errno); }

} // namespace

bool parse_reply(const std::string& line, uint32_t seq, Reply& out) {
    std::istringstream in(line);
    std::string tag;
    long long s = -1;
    if (!(in >> tag >> s) || s != static_cast<long long>(seq)) return false;
    if (tag == "C") {
        long long steer = 0, drive = 0, brake = 0;
        std::string state;
        if (!(in >> steer >> drive >> brake >> state)) return false;
        if (steer < -1 || steer > 1 || drive < -1000 || drive > 1000 || (brake != 0 && brake != 1)) return false;
        out.kind = Reply::Kind::Command;
        out.steer = static_cast<int8_t>(steer);
        out.drive = static_cast<int16_t>(drive);
        out.brake = brake == 1;
        out.state = state;
        return true;
    }
    if (tag == "X") {
        std::string message;
        std::getline(in, message);
        if (!message.empty() && message.front() == ' ') message.erase(0, 1);
        out.kind = Reply::Kind::Error;
        out.state = message;
        return true;
    }
    return false;
}

PyProcess::PyProcess(const std::string& python, const std::string& runner, const std::string& pack_dir,
                     std::chrono::milliseconds start_timeout) {
    int sv[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0) throw std::runtime_error(errno_text("socketpair"));
    // Everything the child needs is prepared before fork: after fork only
    // async-signal-safe calls are made (the core is multi-threaded).
    std::vector<std::string> args = {python, "-I", "-B", "-u", runner, pack_dir};
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);

    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(sv[0]);
        ::close(sv[1]);
        throw std::runtime_error(errno_text("fork"));
    }
    if (pid == 0) {
        ::prctl(PR_SET_PDEATHSIG, SIGKILL);   // never outlive the core
        ::dup2(sv[1], 0);
        ::dup2(sv[1], 1);
        ::close_range(3, ~0U, 0);              // no sockets, GPIO or I2C handles of the core in the child
        ::execv(argv[0], argv.data());
        ::_exit(127);
    }
    ::close(sv[1]);
    fd_ = sv[0];
    pid_ = pid;
    ::fcntl(fd_, F_SETFL, ::fcntl(fd_, F_GETFL) | O_NONBLOCK);

    std::string line;
    const auto deadline = std::chrono::steady_clock::now() + start_timeout;
    while (true) {
        if (!read_line(deadline, line)) {
            const bool gone = !alive();
            stop();
            if (gone) throw std::runtime_error("py_drive: " + python + " " + runner + " ended before drive.py was loaded (see the core's log)");
            throw std::runtime_error("py_drive: drive.py was not loaded within " + std::to_string(start_timeout.count()) + " ms");
        }
        if (line == "READY") return;
        if (line.rfind("ERROR ", 0) == 0) {
            stop();
            throw std::runtime_error(line.substr(6));
        }
        // anything else before READY is ignored
    }
}

PyProcess::~PyProcess() { stop(true); }

bool PyProcess::send_line(const std::string& line) {
    if (fd_ < 0) return false;
    const std::string data = line + "\n";
    const ssize_t n = ::send(fd_, data.data(), data.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
    if (n == static_cast<ssize_t>(data.size())) return true;
    if (n < 0 && (errno == EPIPE || errno == ECONNRESET)) stop();
    return false;   // EAGAIN: the runner is not reading (stuck); treated like a late answer
}

bool PyProcess::read_line(std::chrono::steady_clock::time_point deadline, std::string& line) {
    while (true) {
        const auto nl = buffer_.find('\n');
        if (nl != std::string::npos) {
            line = buffer_.substr(0, nl);
            buffer_.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            return true;
        }
        if (buffer_.size() > kMaxLine) buffer_.clear();
        if (fd_ < 0) return false;
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) return false;
        const int ms = static_cast<int>(std::max<long long>(
            1, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count()));
        pollfd p{fd_, POLLIN, 0};
        const int r = ::poll(&p, 1, ms);
        if (r < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (r == 0) return false;
        char buf[1024];
        const ssize_t n = ::recv(fd_, buf, sizeof buf, 0);
        if (n > 0) {
            buffer_.append(buf, static_cast<size_t>(n));
        } else if (n == 0 || (errno != EAGAIN && errno != EINTR)) {
            stop();   // the runner exited
            return false;
        }
    }
}

bool PyProcess::frame(uint32_t seq, double t_ms, const int32_t tof[3], std::chrono::milliseconds timeout, Reply& out) {
    char line[128];
    std::snprintf(line, sizeof line, "F %u %.1f %d %d %d", seq, t_ms, tof[0], tof[1], tof[2]);
    if (!send_line(line)) return false;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::string reply;
    while (read_line(deadline, reply)) {
        if (parse_reply(reply, seq, out)) return true;
        // an answer to an older frame (it came too late) or a stray line: skip
    }
    return false;
}

bool PyProcess::ping(std::chrono::milliseconds timeout) {
    if (!send_line("P")) return false;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::string reply;
    while (read_line(deadline, reply)) {
        if (reply == "PONG") return true;
    }
    return false;
}

void PyProcess::stop(bool graceful) {
    if (fd_ >= 0) {
        ::close(fd_);   // the runner sees the end of its input and exits
        fd_ = -1;
    }
    buffer_.clear();
    if (pid_ > 0) {
        int status = 0;
        for (int i = 0; graceful && i < 20; ++i) {   // up to 0.1 s to finish by itself
            if (::waitpid(pid_, &status, WNOHANG) == pid_) {
                pid_ = -1;
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        ::kill(pid_, SIGKILL);
        while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {}
        pid_ = -1;
    }
}

} // namespace rumicar::py
