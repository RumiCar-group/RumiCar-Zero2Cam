// What the PC needs to tell what happened to the car during a lost
// connection (docs 5.2, hello): the Linux boot (a new boot id: the car
// restarted), this start of rumicar-core (a new instance id with the same
// boot id: the core restarted), and whether the previous run of the core
// ended through its own shutdown.  A previous run that did not ("unclean")
// was cut by a power loss, a crash or a SIGKILL.
#pragma once

#include <string>

namespace rumicar {

// The kernel's boot id (/proc/sys/kernel/random/boot_id): new at every boot.
// Empty when it cannot be read.
std::string read_boot_id(const std::string& path = "/proc/sys/kernel/random/boot_id");

// 16 random hexadecimal digits for this start of the core.
std::string make_instance_id();

// While this object lives, the marker file exists.  At construction: if the
// file is already there, the previous run did not reach its shutdown
// (previous_exit() is "unclean"), otherwise "clean"; then the file is
// written and flushed to the card (a power loss right after the start still
// leaves it).  The destructor (the core's own shutdown, any return from
// main) removes it and flushes the directory.  The file is held under an
// exclusive lock (flock) while the core runs, so a second rumicar-core
// started by hand next to the service neither takes the running core's
// marker for a cut run nor removes it.  A failure to write or remove, or a
// marker locked by another core, is logged; the core runs either way
// (previous_exit() is then "unknown").
class RunMarker {
public:
    explicit RunMarker(std::string path);
    ~RunMarker();
    RunMarker(const RunMarker&) = delete;
    RunMarker& operator=(const RunMarker&) = delete;

    const std::string& previous_exit() const { return previous_exit_; }

private:
    std::string path_;
    std::string previous_exit_;
    int fd_ = -1;   // the marker, locked while this object lives
};

} // namespace rumicar
