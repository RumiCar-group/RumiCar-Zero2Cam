// Active-version store: which logic and which pack the core should run.
//
// Layout under <state_dir> (default /var/lib/rumicar):
//   logic/<name>-<version>/   plug-in directory (manifest.json + .so)
//   packs/<id>-<version>/     pack directory (manifest.json + data files)
//   current/logic             symlink -> ../logic/<dir>
//   current/pack              symlink -> ../packs/<dir>
//   previous/logic, previous/pack  symlinks kept for rollback (written by rumicar-activate)
//   status/active.json        what the core is running now (written by the core)
//   status/activation.json    result of the last activation attempt (written by the core)
//
// The symlinks are replaced atomically by the rumicar-activate script; the core
// only reads them and reports.
#pragma once

#include <optional>
#include <string>

namespace rumicar {

struct ActiveSelection {
    std::string logic_dir;  // absolute, resolved
    std::string pack_dir;   // absolute, resolved
    bool operator==(const ActiveSelection&) const = default;
};

class VersionStore {
public:
    explicit VersionStore(std::string state_dir);

    // Resolve current/logic and current/pack.  Returns nullopt (and the reason)
    // when either link is missing or dangling.
    std::optional<ActiveSelection> read_current(std::string& reason) const;

    // Write status/active.json atomically.
    void write_active(const std::string& logic_dir, const std::string& logic_version,
                      const std::string& pack_dir, const std::string& pack_version) const;

    // Write status/activation.json atomically.
    void write_activation_result(bool ok, const ActiveSelection& attempted, const std::string& error) const;

    const std::string& state_dir() const { return state_dir_; }

private:
    void write_atomic(const std::string& relative_path, const std::string& content) const;
    std::string state_dir_;
};

} // namespace rumicar
