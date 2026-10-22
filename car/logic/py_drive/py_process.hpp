// RumiCar-Zero2Cam — the user's Python driving program as a child process (py_drive).
//
// The plug-in starts `python3 -I -B -u runner.py <pack dir>` once per
// activation.  runner.py loads the pack's drive.py and answers one line per
// frame over a socket pair (docs 3.12):
//
//   to the runner:    F <seq> <t_ms> <left> <centre> <right>       a frame
//                     P                                            ping (self-test)
//   from the runner:  READY | ERROR <message>                      after loading drive.py
//                     C <seq> <steer> <drive> <brake> <state>      the command for frame <seq>
//                     X <seq> <message>                            drive.py raised an exception
//                     PONG
//
// The control loop never waits long for Python: each frame gets
// step_timeout; a late answer is dropped (its frame is gone) and that frame
// brakes.  A program that stays silent for dead_after, or that exits, is
// stopped for good (every frame brakes until the next activation).  What the
// program prints goes to the core's log (stderr, the journal).
#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <string>

#include <sys/types.h>

namespace rumicar::py {

struct Reply {
    enum class Kind { Command, Error, None } kind = Kind::None;
    int8_t steer = 0;
    int16_t drive = 0;
    bool brake = false;
    std::string state;    // Command: state name ("-" = none); Error: message
};

class PyProcess {
public:
    // Starts the runner and waits for READY.  Throws std::runtime_error with
    // the runner's message (a syntax error in drive.py, ...) on failure.
    PyProcess(const std::string& python, const std::string& runner, const std::string& pack_dir,
              std::chrono::milliseconds start_timeout);
    ~PyProcess();
    PyProcess(const PyProcess&) = delete;
    PyProcess& operator=(const PyProcess&) = delete;

    // Sends a frame and waits up to `timeout` for its command.  Returns false
    // on timeout or when the process is gone (alive() tells which).
    bool frame(uint32_t seq, double t_ms, const int32_t tof[3], std::chrono::milliseconds timeout, Reply& out);

    // Round trip without a frame (self-test).
    bool ping(std::chrono::milliseconds timeout);

    bool alive() const { return fd_ >= 0; }
    pid_t pid() const { return pid_; }

    // Stops the process.  graceful: close its input and give it up to 0.1 s
    // to end by itself (deactivation); otherwise SIGKILL at once (a program
    // that does not answer cannot end by itself, and the control loop must
    // not wait for it).
    void stop(bool graceful = false);

private:
    bool send_line(const std::string& line);
    // Next complete line within the deadline; false on timeout or end of stream.
    bool read_line(std::chrono::steady_clock::time_point deadline, std::string& line);

    int fd_ = -1;
    pid_t pid_ = -1;
    std::string buffer_;
};

// Parses one runner line into a reply for frame `seq`.  Returns false when the
// line is not an answer to that frame (an older frame, or malformed).
bool parse_reply(const std::string& line, uint32_t seq, Reply& out);

} // namespace rumicar::py
