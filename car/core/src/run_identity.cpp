#include "run_identity.hpp"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <unistd.h>
#include <fcntl.h>
#include <sys/file.h>

#include "log.hpp"
#include "util/atomic_file.hpp"

namespace rumicar {

namespace fs = std::filesystem;

std::string read_boot_id(const std::string& path) {
    std::ifstream in(path);
    std::string id;
    std::getline(in, id);
    while (!id.empty() && (id.back() == '\n' || id.back() == '\r' || id.back() == ' ')) id.pop_back();
    return id;
}

std::string make_instance_id() {
    std::random_device rd;
    static const char* hex = "0123456789abcdef";
    std::string id;
    for (int i = 0; i < 4; ++i) {
        const unsigned v = rd();
        for (int s = 28; s >= 16; s -= 4) id.push_back(hex[(v >> s) & 0xF]);
    }
    return id;
}

RunMarker::RunMarker(std::string path) : path_(std::move(path)) {
    bool left = false;
    if (const int old = ::open(path_.c_str(), O_RDONLY | O_CLOEXEC); old >= 0) {
        const bool taken = ::flock(old, LOCK_EX | LOCK_NB) != 0 && errno == EWOULDBLOCK;
        ::close(old);
        if (taken) {
            previous_exit_ = "unknown";
            log::warn("run marker " + path_ + " is held by another rumicar-core");
            return;
        }
        left = true;
    }
    try {
        util::write_file_atomic(path_, std::to_string(::getpid()) + "\n");
    } catch (const std::exception& e) {
        previous_exit_ = "unknown";
        log::warn(std::string("run marker: ") + e.what());
        return;
    }
    fd_ = ::open(path_.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd_ >= 0) ::flock(fd_, LOCK_EX | LOCK_NB);
    previous_exit_ = left ? "unclean" : "clean";
    if (left) log::warn("the previous run of rumicar-core did not end through its shutdown (power loss, crash or SIGKILL)");
}

RunMarker::~RunMarker() {
    if (previous_exit_ == "unknown" && fd_ < 0) return;   // not ours
    if (::unlink(path_.c_str()) != 0 && errno != ENOENT) {
        log::warn("run marker: cannot remove " + path_ + ": " + std::strerror(errno));
    } else {
        const std::string dir = fs::path(path_).parent_path().string();
        const int d = ::open(dir.empty() ? "." : dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (d >= 0) {
            ::fsync(d);   // the removal reaches the card before the power goes
            ::close(d);
        }
    }
    if (fd_ >= 0) ::close(fd_);
}

} // namespace rumicar
