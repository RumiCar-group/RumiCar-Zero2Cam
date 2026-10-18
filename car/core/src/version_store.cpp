#include "version_store.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <unistd.h>

namespace rumicar {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

std::string now_iso8601() {
    const std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    char buf[32];
    std::tm tm{};
    gmtime_r(&t, &tm);
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

} // namespace

VersionStore::VersionStore(std::string state_dir) : state_dir_(std::move(state_dir)) {}

std::optional<ActiveSelection> VersionStore::read_current(std::string& reason) const {
    ActiveSelection sel;
    std::error_code ec;
    for (const char* kind : {"logic", "pack"}) {
        const fs::path link = fs::path(state_dir_) / "current" / kind;
        if (!fs::is_symlink(link, ec)) {
            reason = link.string() + " is not a symlink";
            return std::nullopt;
        }
        const fs::path resolved = fs::canonical(link, ec);
        if (ec || !fs::is_directory(resolved, ec)) {
            reason = link.string() + " does not point to a directory";
            return std::nullopt;
        }
        if (std::string(kind) == "logic") sel.logic_dir = resolved.string();
        else sel.pack_dir = resolved.string();
    }
    reason.clear();
    return sel;
}

void VersionStore::write_active(const std::string& logic_dir, const std::string& logic_version,
                                const std::string& pack_dir, const std::string& pack_version) const {
    json j = {
        {"logic_dir", logic_dir},
        {"logic_version", logic_version},
        {"pack_dir", pack_dir},
        {"pack_version", pack_version},
        {"since", now_iso8601()},
    };
    write_atomic("status/active.json", j.dump(2) + "\n");
}

void VersionStore::write_activation_result(bool ok, const ActiveSelection& attempted, const std::string& error) const {
    json j = {
        {"ok", ok},
        {"logic_dir", attempted.logic_dir},
        {"pack_dir", attempted.pack_dir},
        {"error", error},
        {"at", now_iso8601()},
    };
    write_atomic("status/activation.json", j.dump(2) + "\n");
}

void VersionStore::write_atomic(const std::string& relative_path, const std::string& content) const {
    const fs::path target = fs::path(state_dir_) / relative_path;
    std::error_code ec;
    fs::create_directories(target.parent_path(), ec);
    const fs::path tmp = target.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) throw std::runtime_error("cannot write " + tmp.string());
        out << content;
        out.flush();
        if (!out) throw std::runtime_error("write failed for " + tmp.string());
    }
    // fsync the file before the rename so a power loss cannot leave a truncated target.
    if (FILE* f = std::fopen(tmp.c_str(), "r")) {
        fsync(fileno(f));
        std::fclose(f);
    }
    fs::rename(tmp, target, ec);
    if (ec) throw std::runtime_error("rename failed for " + target.string() + ": " + ec.message());
}

} // namespace rumicar
