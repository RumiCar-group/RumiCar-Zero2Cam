// Loading of driving-logic plug-ins (.so) and creation of logic instances.
#pragma once

#include <memory>
#include <string>

#include "manifest.hpp"
#include "rumicar/logic_api.h"

namespace rumicar {

// A loaded shared library.  Unloaded (dlclose) on destruction; instances
// created from it must be destroyed first.
class LogicLibrary {
public:
    // Verifies <dir>/manifest.json, checks the ABI, and dlopen()s the entry.
    // Throws std::runtime_error.
    explicit LogicLibrary(const std::string& dir);
    ~LogicLibrary();
    LogicLibrary(const LogicLibrary&) = delete;
    LogicLibrary& operator=(const LogicLibrary&) = delete;

    const rumi_logic_api& api() const { return *api_; }
    const Manifest& manifest() const { return manifest_; }
    const std::string& dir() const { return dir_; }

private:
    std::string dir_;
    Manifest manifest_;
    void* handle_ = nullptr;
    const rumi_logic_api* api_ = nullptr;
};

// One logic instance bound to one pack directory.
class LogicInstance {
public:
    // Verifies the pack manifest, checks it targets this logic, creates the
    // instance and runs its self-test.  Throws std::runtime_error.
    LogicInstance(std::shared_ptr<LogicLibrary> lib, const std::string& pack_dir);
    ~LogicInstance();
    LogicInstance(const LogicInstance&) = delete;
    LogicInstance& operator=(const LogicInstance&) = delete;

    // Returns 0 on success (see rumi_logic_api::step).
    int step(const rumi_sensor_frame& frame, rumi_command& out);
    const char* state_name(uint8_t state) const;

    const LogicLibrary& library() const { return *lib_; }
    const Manifest& pack_manifest() const { return pack_manifest_; }
    const std::string& pack_dir() const { return pack_dir_; }

private:
    std::shared_ptr<LogicLibrary> lib_;
    std::string pack_dir_;
    Manifest pack_manifest_;
    void* instance_ = nullptr;
};

} // namespace rumicar
