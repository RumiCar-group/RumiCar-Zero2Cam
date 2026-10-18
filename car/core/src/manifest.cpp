#include "manifest.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include "sha256.hpp"

namespace rumicar {

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

const json& need(const json& j, const char* key) {
    auto it = j.find(key);
    if (it == j.end()) throw std::runtime_error(std::string("manifest: missing key '") + key + "'");
    return *it;
}

std::string need_string(const json& j, const char* key) {
    const json& v = need(j, key);
    if (!v.is_string() || v.get<std::string>().empty())
        throw std::runtime_error(std::string("manifest: '") + key + "' must be a non-empty string");
    return v.get<std::string>();
}

uint32_t need_uint(const json& j, const char* key) {
    const json& v = need(j, key);
    if (!v.is_number_unsigned()) throw std::runtime_error(std::string("manifest: '") + key + "' must be a non-negative integer");
    return v.get<uint32_t>();
}

bool safe_relative_path(const std::string& p) {
    if (p.empty() || p.front() == '/' || p == "manifest.json") return false;
    const fs::path path(p);
    for (const auto& part : path) {
        if (part == ".." || part == ".") return false;
    }
    return true;
}

bool is_hex64(const std::string& s) {
    if (s.size() != 64) return false;
    for (char c : s) {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!ok) return false;
    }
    return true;
}

} // namespace

Manifest parse_manifest(const std::string& json_text) {
    json j;
    try {
        j = json::parse(json_text);
    } catch (const json::exception& e) {
        throw std::runtime_error(std::string("manifest: parse error: ") + e.what());
    }
    if (!j.is_object()) throw std::runtime_error("manifest: top level must be an object");

    Manifest m;
    m.schema = static_cast<int>(need_uint(j, "schema"));
    if (m.schema != 1) throw std::runtime_error("manifest: unsupported schema " + std::to_string(m.schema));

    const std::string kind = need_string(j, "kind");
    if (kind == "pack") {
        m.kind = Manifest::Kind::Pack;
        m.id = need_string(j, "id");
        m.version = need_string(j, "version");
        const json& logic = need(j, "logic");
        if (!logic.is_object()) throw std::runtime_error("manifest: 'logic' must be an object");
        m.logic_name = need_string(logic, "name");
        m.logic_min_abi = need_uint(logic, "min_abi");
    } else if (kind == "logic") {
        m.kind = Manifest::Kind::Logic;
        m.id = need_string(j, "name");
        m.version = need_string(j, "version");
        m.abi = need_uint(j, "abi");
        m.entry = need_string(j, "entry");
        if (!safe_relative_path(m.entry)) throw std::runtime_error("manifest: 'entry' is not a safe relative path");
    } else {
        throw std::runtime_error("manifest: unknown kind '" + kind + "'");
    }
    if (auto it = j.find("created"); it != j.end() && it->is_string()) m.created = it->get<std::string>();

    const json& files = need(j, "files");
    if (!files.is_array() || files.empty()) throw std::runtime_error("manifest: 'files' must be a non-empty array");
    for (const json& f : files) {
        if (!f.is_object()) throw std::runtime_error("manifest: each file entry must be an object");
        ManifestFile mf;
        mf.path = need_string(f, "path");
        if (!safe_relative_path(mf.path)) throw std::runtime_error("manifest: unsafe file path '" + mf.path + "'");
        mf.sha256 = need_string(f, "sha256");
        if (!is_hex64(mf.sha256)) throw std::runtime_error("manifest: bad sha256 for '" + mf.path + "'");
        const json& b = need(f, "bytes");
        if (!b.is_number_unsigned()) throw std::runtime_error("manifest: 'bytes' must be a non-negative integer for '" + mf.path + "'");
        mf.bytes = b.get<uint64_t>();
        for (const auto& prev : m.files) {
            if (prev.path == mf.path) throw std::runtime_error("manifest: duplicate file path '" + mf.path + "'");
        }
        m.files.push_back(std::move(mf));
    }
    if (m.kind == Manifest::Kind::Logic) {
        bool listed = false;
        for (const auto& f : m.files) listed = listed || f.path == m.entry;
        if (!listed) throw std::runtime_error("manifest: 'entry' must be listed in 'files'");
    }
    return m;
}

Manifest load_and_verify_manifest(const std::string& dir) {
    const fs::path base(dir);
    const fs::path mpath = base / "manifest.json";
    std::ifstream in(mpath);
    if (!in) throw std::runtime_error("cannot open " + mpath.string());
    std::stringstream ss;
    ss << in.rdbuf();
    Manifest m = parse_manifest(ss.str());

    for (const auto& f : m.files) {
        const fs::path p = base / f.path;
        std::error_code ec;
        if (!fs::is_regular_file(p, ec)) throw std::runtime_error("manifest: missing file " + p.string());
        const auto size = fs::file_size(p, ec);
        if (ec) throw std::runtime_error("manifest: cannot stat " + p.string());
        if (size != f.bytes)
            throw std::runtime_error("manifest: size mismatch for " + f.path + " (manifest " + std::to_string(f.bytes) +
                                     ", file " + std::to_string(size) + ")");
        const std::string digest = sha256_file_hex(p.string());
        if (digest != f.sha256) throw std::runtime_error("manifest: sha256 mismatch for " + f.path);
    }
    return m;
}

std::string manifest_to_json(const Manifest& m) {
    json j = json::object();
    j["schema"] = 1;
    if (m.kind == Manifest::Kind::Pack) {
        j["kind"] = "pack";
        j["id"] = m.id;
        j["version"] = m.version;
        j["logic"] = {{"name", m.logic_name}, {"min_abi", m.logic_min_abi}};
    } else {
        j["kind"] = "logic";
        j["name"] = m.id;
        j["version"] = m.version;
        j["abi"] = m.abi;
        j["entry"] = m.entry;
    }
    if (!m.created.empty()) j["created"] = m.created;
    j["files"] = json::array();
    for (const auto& f : m.files) j["files"].push_back({{"path", f.path}, {"sha256", f.sha256}, {"bytes", f.bytes}});
    return j.dump(2) + "\n";
}

} // namespace rumicar
