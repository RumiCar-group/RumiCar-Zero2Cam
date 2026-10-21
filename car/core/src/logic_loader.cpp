#include "logic_loader.hpp"

#include <dlfcn.h>

#include <array>
#include <filesystem>
#include <stdexcept>

namespace rumicar {

namespace fs = std::filesystem;

LogicLibrary::LogicLibrary(const std::string& dir) : dir_(dir) {
    manifest_ = load_and_verify_manifest(dir);
    if (manifest_.kind != Manifest::Kind::Logic) throw std::runtime_error(dir + ": manifest is not a logic manifest");
    if (manifest_.abi != RUMI_LOGIC_ABI_VERSION)
        throw std::runtime_error(dir + ": manifest declares ABI " + std::to_string(manifest_.abi) + ", core supports " +
                                 std::to_string(RUMI_LOGIC_ABI_VERSION));

    const fs::path so = fs::path(dir) / manifest_.entry;
    handle_ = dlopen(so.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!handle_) {
        const char* e = dlerror();
        throw std::runtime_error("dlopen failed for " + so.string() + ": " + (e ? e : "unknown error"));
    }
    dlerror();
    void* sym = dlsym(handle_, RUMI_LOGIC_ENTRY_SYMBOL);
    const char* e = dlerror();
    if (e || !sym) {
        dlclose(handle_);
        handle_ = nullptr;
        throw std::runtime_error(so.string() + ": missing symbol " RUMI_LOGIC_ENTRY_SYMBOL);
    }
    auto get_api = reinterpret_cast<rumi_logic_get_api_fn>(sym);
    api_ = get_api();
    if (!api_ || api_->abi_version != RUMI_LOGIC_ABI_VERSION || !api_->name || !api_->version || !api_->create ||
        !api_->destroy || !api_->self_test || !api_->step || !api_->state_name || !api_->aux_name) {
        dlclose(handle_);
        handle_ = nullptr;
        throw std::runtime_error(so.string() + ": plug-in API table is invalid or has a different ABI version");
    }
    if (manifest_.id != api_->name)
        throw std::runtime_error(so.string() + ": manifest name '" + manifest_.id + "' differs from plug-in name '" +
                                 api_->name + "'");
    if (manifest_.version != api_->version)
        throw std::runtime_error(so.string() + ": manifest version '" + manifest_.version +
                                 "' differs from plug-in version '" + api_->version + "'");
}

LogicLibrary::~LogicLibrary() {
    if (handle_) dlclose(handle_);
}

LogicInstance::LogicInstance(std::shared_ptr<LogicLibrary> lib, const std::string& pack_dir)
    : lib_(std::move(lib)), pack_dir_(pack_dir) {
    pack_manifest_ = load_and_verify_manifest(pack_dir);
    if (pack_manifest_.kind != Manifest::Kind::Pack) throw std::runtime_error(pack_dir + ": manifest is not a pack manifest");
    if (pack_manifest_.logic_name != lib_->api().name)
        throw std::runtime_error(pack_dir + ": pack is for logic '" + pack_manifest_.logic_name + "', loaded logic is '" +
                                 lib_->api().name + "'");
    if (pack_manifest_.logic_min_abi > RUMI_LOGIC_ABI_VERSION)
        throw std::runtime_error(pack_dir + ": pack requires ABI " + std::to_string(pack_manifest_.logic_min_abi));

    std::array<char, 512> err{};
    instance_ = lib_->api().create(pack_dir.c_str(), err.data(), err.size());
    if (!instance_) throw std::runtime_error(pack_dir + ": logic create failed: " + err.data());
    err.fill('\0');
    const int rc = lib_->api().self_test(instance_, err.data(), err.size());
    if (rc != 0) {
        lib_->api().destroy(instance_);
        instance_ = nullptr;
        throw std::runtime_error(pack_dir + ": logic self-test failed (" + std::to_string(rc) + "): " + err.data());
    }
}

LogicInstance::~LogicInstance() {
    if (instance_) lib_->api().destroy(instance_);
}

int LogicInstance::step(const rumi_sensor_frame& frame, rumi_command& out) {
    return lib_->api().step(instance_, &frame, &out);
}

const char* LogicInstance::state_name(uint8_t state) const {
    const char* n = lib_->api().state_name(instance_, state);
    return n ? n : "?";
}

std::vector<std::string> LogicInstance::aux_names() const {
    std::vector<std::string> names;
    for (unsigned i = 0; i < 255; ++i) {
        const char* n = lib_->api().aux_name(instance_, static_cast<uint8_t>(i));
        if (!n) break;
        names.emplace_back(n);
    }
    return names;
}

} // namespace rumicar
