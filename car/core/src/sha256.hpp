// SHA-256 helpers (OpenSSL EVP).
#pragma once

#include <string>

namespace rumicar {

// Lower-case hex digest of the file's contents.  Throws std::runtime_error.
std::string sha256_file_hex(const std::string& path);

// Lower-case hex digest of a byte string.
std::string sha256_hex(const std::string& data);

} // namespace rumicar
