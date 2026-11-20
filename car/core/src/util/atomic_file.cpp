#include "atomic_file.hpp"

#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace rumicar::util {

namespace fs = std::filesystem;

namespace {

void fsync_path(const fs::path& p) {
    if (FILE* f = std::fopen(p.c_str(), "r")) {
        fsync(fileno(f));
        std::fclose(f);
    }
}

} // namespace

void write_file_atomic(const std::string& path, const std::string& content) {
    const fs::path target(path);
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
    fsync_path(tmp);
    fs::rename(tmp, target, ec);
    if (ec) throw std::runtime_error("rename failed for " + target.string() + ": " + ec.message());
    fsync_path(target.parent_path());
}

} // namespace rumicar::util
