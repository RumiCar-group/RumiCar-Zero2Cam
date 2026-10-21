#include "version_store.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <unistd.h>

#include "manifest.hpp"

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

void fsync_dir(const fs::path& dir) {
    if (FILE* f = std::fopen(dir.c_str(), "r")) {
        fsync(fileno(f));
        std::fclose(f);
    }
}

} // namespace

VersionStore::VersionStore(std::string state_dir) : state_dir_(std::move(state_dir)) {}

std::string VersionStore::subdir_for(const std::string& kind) {
    if (kind == "logic") return "logic";
    if (kind == "pack") return "packs";
    throw std::runtime_error("kind must be 'logic' or 'pack'");
}

bool VersionStore::is_plain_dir_name(const std::string& name) {
    if (name.empty() || name[0] == '.') return false;
    for (const char c : name) {
        if (c == '/' || c == '\\' || c == '\0') return false;
    }
    return true;
}

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

std::string VersionStore::link_target(const char* which, const char* kind) const {
    const fs::path link = fs::path(state_dir_) / which / kind;
    std::error_code ec;
    if (!fs::is_symlink(link, ec)) return "";
    const fs::path target = fs::read_symlink(link, ec);
    if (ec) return "";
    return target.filename().string();
}

std::string VersionStore::resolve_link(const char* which, const char* kind) const {
    const fs::path link = fs::path(state_dir_) / which / kind;
    std::error_code ec;
    if (!fs::is_symlink(link, ec)) return "";
    const fs::path resolved = fs::canonical(link, ec);
    if (ec || !fs::is_directory(resolved, ec)) return "";
    return resolved.string();
}

void VersionStore::link_atomic(const std::string& kind, const std::string& relative_target, bool keep_previous) const {
    const fs::path current_dir = fs::path(state_dir_) / "current";
    const fs::path previous_dir = fs::path(state_dir_) / "previous";
    std::error_code ec;
    fs::create_directories(current_dir, ec);
    fs::create_directories(previous_dir, ec);

    const fs::path current = current_dir / kind;
    if (keep_previous && fs::is_symlink(current, ec)) {
        const fs::path old = fs::read_symlink(current, ec);
        if (!ec) {
            const fs::path tmp = previous_dir / ("." + kind + ".tmp");
            fs::remove(tmp, ec);
            fs::create_symlink(old, tmp, ec);
            if (ec) throw std::runtime_error("cannot write previous/" + kind + ": " + ec.message());
            fs::rename(tmp, previous_dir / kind, ec);
            if (ec) throw std::runtime_error("cannot replace previous/" + kind + ": " + ec.message());
        }
    }
    const fs::path tmp = current_dir / ("." + kind + ".tmp");
    fs::remove(tmp, ec);
    fs::create_symlink(relative_target, tmp, ec);
    if (ec) throw std::runtime_error("cannot write current/" + kind + ": " + ec.message());
    fs::rename(tmp, current, ec);
    if (ec) throw std::runtime_error("cannot replace current/" + kind + ": " + ec.message());
    fsync_dir(current_dir);
    fsync_dir(previous_dir);
}

void VersionStore::activate(const std::string& kind, const std::string& dir_name) const {
    const std::string sub = subdir_for(kind);
    if (!is_plain_dir_name(dir_name)) throw std::runtime_error("bad directory name '" + dir_name + "'");
    const fs::path dir = fs::path(state_dir_) / sub / dir_name;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) throw std::runtime_error(dir.string() + " does not exist");
    if (!fs::is_regular_file(dir / "manifest.json", ec)) throw std::runtime_error(dir.string() + " has no manifest.json");
    link_atomic(kind, "../" + sub + "/" + dir_name, true);
}

void VersionStore::rollback(const std::string& kind) const {
    subdir_for(kind); // validates kind
    const fs::path previous = fs::path(state_dir_) / "previous" / kind;
    std::error_code ec;
    if (!fs::is_symlink(previous, ec)) throw std::runtime_error("no previous " + kind);
    const fs::path target = fs::read_symlink(previous, ec);
    if (ec) throw std::runtime_error("cannot read previous/" + kind + ": " + ec.message());
    link_atomic(kind, target.string(), true);
}

void VersionStore::restore_current(const std::string& kind, const std::string& running_dir) const {
    const std::string sub = subdir_for(kind);
    const fs::path dir(running_dir);
    const std::string name = dir.filename().string();
    if (!is_plain_dir_name(name)) throw std::runtime_error("bad running directory '" + running_dir + "'");
    std::error_code ec;
    const fs::path expected = fs::canonical(fs::path(state_dir_) / sub / name, ec);
    if (ec || expected != fs::canonical(dir, ec)) throw std::runtime_error(running_dir + " is not under " + sub + "/");
    link_atomic(kind, "../" + sub + "/" + name, false);
}

void VersionStore::remove(const std::string& kind, const std::string& dir_name) const {
    const std::string sub = subdir_for(kind);
    if (!is_plain_dir_name(dir_name)) throw std::runtime_error("bad directory name '" + dir_name + "'");
    if (link_target("current", kind.c_str()) == dir_name) throw std::runtime_error(dir_name + " is the current " + kind);
    if (link_target("previous", kind.c_str()) == dir_name) throw std::runtime_error(dir_name + " is the previous " + kind + " (kept for rollback)");
    const fs::path dir = fs::path(state_dir_) / sub / dir_name;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) throw std::runtime_error(sub + "/" + dir_name + " does not exist");
    fs::remove_all(dir, ec);
    if (ec) throw std::runtime_error("cannot remove " + dir.string() + ": " + ec.message());
}

std::vector<InstalledVersion> VersionStore::list(const std::string& kind) const {
    const fs::path root = fs::path(state_dir_) / subdir_for(kind);
    std::vector<InstalledVersion> out;
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return out;
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (!entry.is_directory(ec)) continue;
        InstalledVersion v;
        v.dir_name = entry.path().filename().string();
        if (!is_plain_dir_name(v.dir_name)) continue;
        try {
            const Manifest m = load_and_verify_manifest(entry.path().string());
            const bool kind_matches = (kind == "logic") == (m.kind == Manifest::Kind::Logic);
            v.id = m.id;
            v.version = m.version;
            v.description = m.description;
            v.capabilities = m.capabilities;
            v.scenarios = m.scenarios;
            v.parent = m.parent;
            if (!kind_matches) {
                v.error = "manifest kind does not match the directory";
            } else if (m.dir_name() != v.dir_name) {
                v.error = "directory name does not match manifest (" + m.dir_name() + ")";
            } else {
                v.verified = true;
            }
        } catch (const std::exception& e) {
            v.error = e.what();
        }
        out.push_back(std::move(v));
    }
    std::sort(out.begin(), out.end(), [](const InstalledVersion& a, const InstalledVersion& b) { return a.dir_name < b.dir_name; });
    return out;
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
