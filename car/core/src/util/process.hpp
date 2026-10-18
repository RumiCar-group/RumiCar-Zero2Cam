// Running an external program (no shell) with a timeout and captured output.
#pragma once

#include <string>
#include <vector>

namespace rumicar::util {

struct ProcessResult {
    int exit_code = -1;        // -1 when the program could not be started or was killed
    bool timed_out = false;
    std::string output;        // stdout and stderr, interleaved
};

// argv[0] is the program path (absolute).  Throws std::runtime_error only
// when the process cannot be created at all.
ProcessResult run_process(const std::vector<std::string>& argv, int timeout_ms);

} // namespace rumicar::util
