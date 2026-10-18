#include "process.hpp"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <stdexcept>
#include <sys/wait.h>
#include <unistd.h>

namespace rumicar::util {

ProcessResult run_process(const std::vector<std::string>& argv, int timeout_ms) {
    if (argv.empty()) throw std::runtime_error("run_process: empty argv");
    int pipefd[2];
    if (::pipe2(pipefd, O_CLOEXEC) != 0) throw std::runtime_error(std::string("pipe: ") + std::strerror(errno));

    std::vector<char*> args;
    args.reserve(argv.size() + 1);
    for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);

    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        throw std::runtime_error(std::string("fork: ") + std::strerror(errno));
    }
    if (pid == 0) {
        // child
        ::dup2(pipefd[1], STDOUT_FILENO);
        ::dup2(pipefd[1], STDERR_FILENO);
        const int devnull = ::open("/dev/null", O_RDONLY);
        if (devnull >= 0) ::dup2(devnull, STDIN_FILENO);
        ::execv(args[0], args.data());
        const char msg[] = "exec failed\n";
        const ssize_t ignored = ::write(STDERR_FILENO, msg, sizeof msg - 1);
        (void)ignored;
        ::_exit(127);
    }
    ::close(pipefd[1]);

    ProcessResult r;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    char buf[1024];
    bool open = true;
    while (open) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0) {
            r.timed_out = true;
            ::kill(pid, SIGKILL);
            break;
        }
        pollfd pfd{pipefd[0], POLLIN, 0};
        const int rc = ::poll(&pfd, 1, static_cast<int>(left));
        if (rc < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (rc == 0) continue;   // loop re-checks the deadline
        const ssize_t n = ::read(pipefd[0], buf, sizeof buf);
        if (n > 0) {
            r.output.append(buf, static_cast<size_t>(n));
            if (r.output.size() > 64 * 1024) r.output.erase(0, r.output.size() - 64 * 1024);
        } else if (n == 0 || (n < 0 && errno != EINTR)) {
            open = false;
        }
    }
    ::close(pipefd[0]);

    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (!r.timed_out && WIFEXITED(status)) r.exit_code = WEXITSTATUS(status);
    return r;
}

} // namespace rumicar::util
