// Active-version store: which logic and which pack the core should run.
//
// Layout under <state_dir> (default /var/lib/rumicar):
//   logic/<name>-<version>/   plug-in directory (manifest.json + .so)
//   packs/<id>-<version>/     pack directory (manifest.json + data files)
//   current/logic             symlink -> ../logic/<dir>
//   current/pack              symlink -> ../packs/<dir>
//   previous/logic, previous/pack  symlinks kept for rollback
//   status/active.json        what the core is running now (written by the core)
//   status/activation.json    result of the last activation attempt (written by the core)
//
// The symlinks are replaced atomically (temporary link + rename), either by
// the rumicar-activate script over SSH or by the control server (activate /
// rollback requests); both do exactly the same thing.
#pragma once

#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace rumicar {

struct ActiveSelection {
    std::string logic_dir;  // absolute, resolved
    std::string pack_dir;   // absolute, resolved
    bool operator==(const ActiveSelection&) const = default;
};

// One installed logic or pack directory, as reported by list_versions.
struct InstalledVersion {
    std::string dir_name;   // e.g. base-tof-0.1.0
    std::string id;         // manifest id (pack) or name (logic); empty when unreadable
    std::string version;    // empty when unreadable
    bool verified = false;  // manifest present and every file matches
    std::string error;      // why verification failed
    // Optional manifest metadata (empty when absent or unreadable).
    std::string description;
    std::vector<std::string> capabilities;
    std::vector<std::string> scenarios;
    std::string parent;
    std::string logic_name;   // pack: the logic it is for (manifest logic.name)
};

class VersionStore {
public:
    explicit VersionStore(std::string state_dir);

    // Resolve current/logic and current/pack.  Returns nullopt (and the reason)
    // when either link is missing or dangling.
    std::optional<ActiveSelection> read_current(std::string& reason) const;

    // Target of current/<kind> or previous/<kind> as a directory name
    // (e.g. "base-tof-0.1.0"), or empty when the link is not set.
    std::string link_target(const char* which, const char* kind) const;

    // Resolved absolute directory behind current/<kind> or previous/<kind>,
    // or empty when the link is missing or dangling.
    std::string resolve_link(const char* which, const char* kind) const;

    // Point current/<kind> at <sub>/<dir_name> (kind: "logic" or "pack").
    // Validates the name and the presence of manifest.json, keeps the old
    // target in previous/<kind>.  Throws std::runtime_error.
    void activate(const std::string& kind, const std::string& dir_name) const;

    // Point current/logic and current/pack at a matching pair in one step:
    // switching between logics (tof_drive, cam_drive, py_drive) needs both,
    // and the control loop never sees one without the other (both links are
    // written under the store's lock, which read_current also takes).
    void activate_pair(const std::string& logic_dir_name, const std::string& pack_dir_name) const;

    // Swap current/<kind> and previous/<kind>.  Throws std::runtime_error.
    void rollback(const std::string& kind) const;
    // Swap both current/ and previous/ links in one step (undoes activate_pair).
    void rollback_pair() const;

    // Point current/<kind> back at the directory that is actually running
    // (absolute path) without touching previous/<kind>.  Used by the core
    // after a failed activation so that a reboot starts the working versions.
    void restore_current(const std::string& kind, const std::string& running_dir) const;

    // All directories under logic/ or packs/ with their verification result.
    std::vector<InstalledVersion> list(const std::string& kind) const;

    // Delete an installed version that is neither current nor previous.
    // Throws std::runtime_error.
    void remove(const std::string& kind, const std::string& dir_name) const;

    // Write status/active.json atomically.
    void write_active(const std::string& logic_dir, const std::string& logic_version,
                      const std::string& pack_dir, const std::string& pack_version) const;

    // Write status/activation.json atomically.
    void write_activation_result(bool ok, const ActiveSelection& attempted, const std::string& error) const;

    const std::string& state_dir() const { return state_dir_; }

    // "logic" -> "logic", "pack" -> "packs"; throws for anything else.
    static std::string subdir_for(const std::string& kind);
    // A plain directory name: non-empty, no '/', '\\', NUL, not starting with '.'.
    static bool is_plain_dir_name(const std::string& name);

private:
    void link_atomic(const std::string& kind, const std::string& relative_target, bool keep_previous) const;
    void write_atomic(const std::string& relative_path, const std::string& content) const;
    std::string checked_target(const std::string& kind, const std::string& dir_name) const;
    std::string previous_target(const std::string& kind) const;
    std::string state_dir_;
    // Link changes and read_current are serialised, so a pair is never seen half-written.
    mutable std::mutex mutex_;
};

} // namespace rumicar
