// Receiving a logic or pack directory from the PC over the control channel
// (docs 5.2: upload_begin / upload_chunk / upload_end / upload_abort).
//
// Files are written into <state_dir>/incoming/<kind>-<name>/ and only moved
// to <state_dir>/<logic|packs>/<name>/ after every declared file has arrived
// with the declared size and SHA-256 and the manifest verifies, so the
// version store never sees a half-written directory.  One upload at a time.
#pragma once

#include <cstdint>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace rumicar {

class UploadManager {
public:
    static constexpr size_t kChunkBytes = 32 * 1024;        // payload per upload_chunk (before base64)
    static constexpr uint64_t kMaxTotalBytes = 64ull * 1024 * 1024;
    static constexpr size_t kMaxFiles = 200;

    explicit UploadManager(std::string state_dir);
    ~UploadManager();

    // Removes leftovers of interrupted uploads (<state_dir>/incoming).
    void clean_incoming();

    // params: kind, name, files [{path, bytes, sha256}].  Returns the result
    // for the PC.  Throws std::runtime_error with the message for ok=false.
    nlohmann::json begin(const nlohmann::json& params);
    // params: path, offset, data (base64).  Chunks of a file must arrive in order.
    nlohmann::json chunk(const nlohmann::json& params);
    // Verifies and installs; returns {name, id, version}.
    nlohmann::json end();
    // Discards the current upload (no error when none is active).
    void abort();

    bool active() const { return session_ != nullptr; }

private:
    struct FileEntry {
        std::string path;
        uint64_t bytes = 0;
        std::string sha256;
        uint64_t received = 0;
        bool complete = false;
    };
    struct Session {
        std::string kind;
        std::string name;
        std::string staging;     // absolute directory
        std::map<std::string, FileEntry> files;
        std::string open_path;   // file currently being written
        std::ofstream out;
        uint64_t total = 0;
        uint64_t received_total = 0;
    };

    std::string state_dir_;
    std::unique_ptr<Session> session_;
};

// Base64 decoding (standard alphabet, '=' padding).  Throws std::runtime_error on bad input.
std::vector<uint8_t> base64_decode(const std::string& text);

} // namespace rumicar
