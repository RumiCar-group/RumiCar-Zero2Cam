// Replacing a small file so that a reader or a power loss never sees it half
// written: the content goes to <path>.tmp, which is flushed to the card and
// renamed over <path>; the directory is flushed after the rename.
#pragma once

#include <string>

namespace rumicar::util {

// Creates the parent directories as needed.  Throws std::runtime_error.
void write_file_atomic(const std::string& path, const std::string& content);

} // namespace rumicar::util
