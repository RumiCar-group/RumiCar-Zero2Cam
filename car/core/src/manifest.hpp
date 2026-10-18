// Manifest handling for packs (decision data) and logic plug-ins.
//
// Every deployable directory carries a manifest.json that lists its files with
// SHA-256 digests.  The core refuses to activate a directory whose manifest is
// missing, malformed, or does not match the files on disk.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rumicar {

struct ManifestFile {
    std::string path;    // relative to the manifest directory
    std::string sha256;  // lower-case hex
    uint64_t    bytes = 0;
};

struct Manifest {
    enum class Kind { Pack, Logic };
    int         schema = 0;
    Kind        kind = Kind::Pack;
    std::string id;        // pack: "id"; logic: "name"
    std::string version;
    std::string created;   // ISO-8601, informational
    // pack only
    std::string logic_name;
    uint32_t    logic_min_abi = 0;
    // logic only
    uint32_t    abi = 0;
    std::string entry;     // shared library file name
    std::vector<ManifestFile> files;

    // Directory name under <state_dir>/packs or <state_dir>/logic.
    std::string dir_name() const { return id + "-" + version; }
};

// Parse manifest.json text.  Throws std::runtime_error with a precise message.
Manifest parse_manifest(const std::string& json_text);

// Load <dir>/manifest.json and verify every listed file (existence, size, digest).
// Throws std::runtime_error naming the first problem found.
Manifest load_and_verify_manifest(const std::string& dir);

// Serialise a manifest (used by tooling and tests).
std::string manifest_to_json(const Manifest& m);

} // namespace rumicar
