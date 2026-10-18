#include "upload.hpp"

#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <unistd.h>

#include "log.hpp"
#include "manifest.hpp"
#include "sha256.hpp"
#include "version_store.hpp"

namespace rumicar {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

bool is_hex64(const std::string& s) {
    if (s.size() != 64) return false;
    for (const char c : s) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

void fsync_path(const fs::path& p) {
    if (FILE* f = std::fopen(p.c_str(), "r")) {
        fsync(fileno(f));
        std::fclose(f);
    }
}

} // namespace

std::vector<uint8_t> base64_decode(const std::string& text) {
    static constexpr auto value = [](const char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    if (text.size() % 4 != 0) throw std::runtime_error("base64: length is not a multiple of 4");
    std::vector<uint8_t> out;
    out.reserve(text.size() / 4 * 3);
    for (size_t i = 0; i < text.size(); i += 4) {
        int v[4];
        int pad = 0;
        for (size_t k = 0; k < 4; ++k) {
            const char c = text[i + k];
            if (c == '=') {
                if (i + 4 != text.size() || k < 2) throw std::runtime_error("base64: misplaced padding");
                v[k] = 0;
                ++pad;
            } else {
                if (pad > 0) throw std::runtime_error("base64: data after padding");
                v[k] = value(c);
                if (v[k] < 0) throw std::runtime_error("base64: invalid character");
            }
        }
        const uint32_t triple = (static_cast<uint32_t>(v[0]) << 18) | (static_cast<uint32_t>(v[1]) << 12) |
                                (static_cast<uint32_t>(v[2]) << 6) | static_cast<uint32_t>(v[3]);
        out.push_back(static_cast<uint8_t>((triple >> 16) & 0xFF));
        if (pad < 2) out.push_back(static_cast<uint8_t>((triple >> 8) & 0xFF));
        if (pad < 1) out.push_back(static_cast<uint8_t>(triple & 0xFF));
    }
    return out;
}

UploadManager::UploadManager(std::string state_dir) : state_dir_(std::move(state_dir)) {}

UploadManager::~UploadManager() { abort(); }

void UploadManager::clean_incoming() {
    std::error_code ec;
    const fs::path incoming = fs::path(state_dir_) / "incoming";
    if (fs::is_directory(incoming, ec)) {
        const auto removed = fs::remove_all(incoming, ec);
        if (removed > 0) log::info("removed " + std::to_string(removed) + " leftover upload entries");
    }
}

json UploadManager::begin(const json& params) {
    if (session_) throw std::runtime_error("another upload is in progress (send upload_abort first)");

    const auto kind_it = params.find("kind");
    if (kind_it == params.end() || !kind_it->is_string()) throw std::runtime_error("missing string 'kind'");
    const std::string kind = kind_it->get<std::string>();
    const std::string sub = VersionStore::subdir_for(kind);   // validates kind
    const auto name_it = params.find("name");
    if (name_it == params.end() || !name_it->is_string()) throw std::runtime_error("missing string 'name'");
    const std::string name = name_it->get<std::string>();
    if (!VersionStore::is_plain_dir_name(name)) throw std::runtime_error("bad directory name '" + name + "'");
    const auto files_it = params.find("files");
    if (files_it == params.end() || !files_it->is_array() || files_it->empty()) throw std::runtime_error("'files' must be a non-empty array");
    if (files_it->size() > kMaxFiles) throw std::runtime_error("too many files (max " + std::to_string(kMaxFiles) + ")");

    const fs::path target = fs::path(state_dir_) / sub / name;
    std::error_code ec;
    if (fs::exists(target, ec)) throw std::runtime_error(sub + "/" + name + " is already installed; use a new version");

    auto s = std::make_unique<Session>();
    s->kind = kind;
    s->name = name;
    bool has_manifest = false;
    for (const auto& f : *files_it) {
        if (!f.is_object()) throw std::runtime_error("'files' entries must be objects");
        FileEntry e;
        const auto p = f.find("path");
        const auto b = f.find("bytes");
        const auto h = f.find("sha256");
        if (p == f.end() || !p->is_string()) throw std::runtime_error("file entry: missing string 'path'");
        if (b == f.end() || !b->is_number_unsigned()) throw std::runtime_error("file entry: missing non-negative 'bytes'");
        if (h == f.end() || !h->is_string() || !is_hex64(h->get<std::string>())) throw std::runtime_error("file entry: bad 'sha256'");
        e.path = p->get<std::string>();
        e.bytes = b->get<uint64_t>();
        e.sha256 = h->get<std::string>();
        if (e.path == "manifest.json") has_manifest = true;
        else if (!is_safe_relative_path(e.path)) throw std::runtime_error("unsafe file path '" + e.path + "'");
        if (s->files.count(e.path)) throw std::runtime_error("duplicate file path '" + e.path + "'");
        s->total += e.bytes;
        if (s->total > kMaxTotalBytes) throw std::runtime_error("upload larger than " + std::to_string(kMaxTotalBytes / (1024 * 1024)) + " MiB");
        s->files[e.path] = std::move(e);
    }
    if (!has_manifest) throw std::runtime_error("'files' must include manifest.json");

    s->staging = (fs::path(state_dir_) / "incoming" / (kind + "-" + name)).string();
    fs::remove_all(s->staging, ec);
    fs::create_directories(s->staging, ec);
    if (ec) throw std::runtime_error("cannot create " + s->staging + ": " + ec.message());
    // Create every file (empty files never get a chunk).
    for (const auto& [path, e] : s->files) {
        const fs::path full = fs::path(s->staging) / path;
        fs::create_directories(full.parent_path(), ec);
        std::ofstream touch(full, std::ios::binary | std::ios::trunc);
        if (!touch) throw std::runtime_error("cannot create " + full.string());
    }
    session_ = std::move(s);
    log::info("upload started: " + kind + " " + name + " (" + std::to_string(session_->files.size()) + " files, " +
              std::to_string(session_->total) + " bytes)");
    return json{{"chunk_bytes", kChunkBytes}, {"name", name}};
}

json UploadManager::chunk(const json& params) {
    if (!session_) throw std::runtime_error("no upload in progress");
    Session& s = *session_;
    const auto p = params.find("path");
    const auto o = params.find("offset");
    const auto d = params.find("data");
    if (p == params.end() || !p->is_string()) throw std::runtime_error("missing string 'path'");
    if (o == params.end() || !o->is_number_unsigned()) throw std::runtime_error("missing non-negative 'offset'");
    if (d == params.end() || !d->is_string()) throw std::runtime_error("missing string 'data'");
    const std::string path = p->get<std::string>();
    const auto it = s.files.find(path);
    if (it == s.files.end()) throw std::runtime_error("file '" + path + "' was not declared in upload_begin");
    FileEntry& e = it->second;
    if (e.complete) throw std::runtime_error("file '" + path + "' is already complete");
    if (o->get<uint64_t>() != e.received)
        throw std::runtime_error("offset " + std::to_string(o->get<uint64_t>()) + " does not match the " + std::to_string(e.received) +
                                 " bytes received for '" + path + "'");
    const std::vector<uint8_t> data = base64_decode(d->get<std::string>());
    if (data.size() > kChunkBytes) throw std::runtime_error("chunk larger than " + std::to_string(kChunkBytes) + " bytes");
    if (e.received + data.size() > e.bytes) throw std::runtime_error("more data than declared for '" + path + "'");

    if (s.open_path != path) {
        if (s.out.is_open()) s.out.close();
        s.out.open(fs::path(s.staging) / path, std::ios::binary | std::ios::app);
        if (!s.out) throw std::runtime_error("cannot open '" + path + "' for writing");
        s.open_path = path;
    }
    s.out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!s.out) throw std::runtime_error("write failed for '" + path + "'");
    e.received += data.size();
    s.received_total += data.size();
    if (e.received == e.bytes) {
        s.out.flush();
        e.complete = true;
    }
    return json{{"received", e.received}, {"bytes", e.bytes}};
}

json UploadManager::end() {
    if (!session_) throw std::runtime_error("no upload in progress");
    std::unique_ptr<Session> s = std::move(session_);
    if (s->out.is_open()) s->out.close();
    try {
        for (auto& [path, e] : s->files) {
            if (e.received != e.bytes) {
                throw std::runtime_error("file '" + path + "' is incomplete (" + std::to_string(e.received) + " of " +
                                         std::to_string(e.bytes) + " bytes)");
            }
            const fs::path full = fs::path(s->staging) / path;
            if (sha256_file_hex(full.string()) != e.sha256) throw std::runtime_error("sha256 mismatch for '" + path + "'");
            fsync_path(full);
        }
        const Manifest m = load_and_verify_manifest(s->staging);
        const bool kind_ok = (s->kind == "logic") == (m.kind == Manifest::Kind::Logic);
        if (!kind_ok) throw std::runtime_error("manifest kind does not match '" + s->kind + "'");
        if (m.dir_name() != s->name)
            throw std::runtime_error("directory name '" + s->name + "' does not match the manifest (" + m.dir_name() + ")");
        for (const auto& [path, e] : s->files) {
            if (path == "manifest.json") continue;
            bool listed = false;
            for (const auto& mf : m.files) listed = listed || mf.path == path;
            if (!listed) throw std::runtime_error("uploaded file '" + path + "' is not listed in the manifest");
        }

        const fs::path target = fs::path(state_dir_) / VersionStore::subdir_for(s->kind) / s->name;
        std::error_code ec;
        if (fs::exists(target, ec)) throw std::runtime_error(target.string() + " appeared during the upload");
        fs::create_directories(target.parent_path(), ec);
        fs::rename(s->staging, target, ec);
        if (ec) throw std::runtime_error("cannot install " + target.string() + ": " + ec.message());
        fsync_path(target.parent_path());
        log::info("upload installed: " + s->kind + " " + s->name + " (" + m.id + " " + m.version + ")");
        return json{{"name", s->name}, {"id", m.id}, {"version", m.version}};
    } catch (...) {
        std::error_code ec;
        fs::remove_all(s->staging, ec);
        throw;
    }
}

void UploadManager::abort() {
    if (!session_) return;
    std::unique_ptr<Session> s = std::move(session_);
    if (s->out.is_open()) s->out.close();
    std::error_code ec;
    fs::remove_all(s->staging, ec);
    log::info("upload aborted: " + s->kind + " " + s->name);
}

} // namespace rumicar
