// Tests for manifest parsing/verification, logic loading and core.json parsing.
// Usage: test_pack <base pack dir> <path to libtof_drive.so>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>

#include "check.hpp"
#include "config.hpp"
#include "logic_loader.hpp"
#include "manifest.hpp"
#include "sha256.hpp"

using namespace rumicar;
namespace fs = std::filesystem;

namespace {

std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void write_file(const fs::path& p, const std::string& content) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << content;
}

bool throws(const std::function<void()>& f) {
    try { f(); } catch (const std::exception&) { return true; }
    return false;
}

void test_sha256_known_vector() {
    // SHA-256("abc")
    CHECK(sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(sha256_hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

void test_committed_base_pack_is_consistent(const fs::path& pack_dir) {
    // The pack committed in the repository must verify as-is.
    Manifest m = load_and_verify_manifest(pack_dir.string());
    CHECK(m.kind == Manifest::Kind::Pack);
    CHECK(m.id == "base-tof");
    CHECK(m.logic_name == "tof_drive");
    CHECK(m.logic_min_abi == RUMI_LOGIC_ABI_VERSION);
    CHECK(m.files.size() == 1 && m.files[0].path == "params.json");
}

void test_manifest_rejections(const fs::path& tmp) {
    const fs::path d = tmp / "bad";
    fs::remove_all(d);
    write_file(d / "params.json", "{}");
    const std::string sha = sha256_file_hex((d / "params.json").string());

    auto manifest_with = [&](const std::string& sha_val, uint64_t bytes, const std::string& path = "params.json") {
        return std::string("{\"schema\":1,\"kind\":\"pack\",\"id\":\"x\",\"version\":\"1\",\"logic\":{\"name\":\"tof_drive\",\"min_abi\":1},"
                           "\"files\":[{\"path\":\"") + path + "\",\"sha256\":\"" + sha_val + "\",\"bytes\":" + std::to_string(bytes) + "}]}";
    };

    write_file(d / "manifest.json", manifest_with(sha, 2));
    CHECK(!throws([&] { load_and_verify_manifest(d.string()); }));

    write_file(d / "manifest.json", manifest_with(std::string(64, '0'), 2));
    CHECK_MSG(throws([&] { load_and_verify_manifest(d.string()); }), "digest mismatch must be rejected");

    write_file(d / "manifest.json", manifest_with(sha, 3));
    CHECK_MSG(throws([&] { load_and_verify_manifest(d.string()); }), "size mismatch must be rejected");

    write_file(d / "manifest.json", manifest_with(sha, 2, "../params.json"));
    CHECK_MSG(throws([&] { load_and_verify_manifest(d.string()); }), "path traversal must be rejected");

    write_file(d / "manifest.json", manifest_with(sha, 2, "missing.json"));
    CHECK_MSG(throws([&] { load_and_verify_manifest(d.string()); }), "missing file must be rejected");

    CHECK(throws([&] { parse_manifest("{\"schema\":2,\"kind\":\"pack\"}"); }));
    CHECK(throws([&] { parse_manifest("not json"); }));
    CHECK(throws([&] { parse_manifest("{\"schema\":1,\"kind\":\"logic\",\"name\":\"a\",\"version\":\"1\",\"abi\":1,\"entry\":\"liba.so\",\"files\":[]}"); }));

    Manifest round;
    round.kind = Manifest::Kind::Logic;
    round.id = "tof_drive";
    round.version = "0.1.0";
    round.abi = 1;
    round.entry = "libtof_drive.so";
    round.files.push_back({"libtof_drive.so", std::string(64, 'a'), 10});
    Manifest back = parse_manifest(manifest_to_json(round));
    CHECK(back.kind == Manifest::Kind::Logic && back.id == "tof_drive" && back.entry == "libtof_drive.so" && back.abi == 1);
}

void test_logic_library_loads(const fs::path& tmp, const fs::path& so, const fs::path& pack_dir) {
    // Build a logic directory around the freshly built plug-in and load it.
    const fs::path d = tmp / "logic" / "tof_drive-test";
    fs::remove_all(d);
    fs::create_directories(d);
    fs::copy_file(so, d / "libtof_drive.so", fs::copy_options::overwrite_existing);
    Manifest m;
    m.kind = Manifest::Kind::Logic;
    m.id = "tof_drive";
    m.version = "0.1.0";
    m.abi = RUMI_LOGIC_ABI_VERSION;
    m.entry = "libtof_drive.so";
    m.files.push_back({"libtof_drive.so", sha256_file_hex((d / "libtof_drive.so").string()), fs::file_size(d / "libtof_drive.so")});
    write_file(d / "manifest.json", manifest_to_json(m));

    auto lib = std::make_shared<LogicLibrary>(d.string());
    CHECK(std::string(lib->api().name) == "tof_drive");
    CHECK(lib->api().abi_version == RUMI_LOGIC_ABI_VERSION);

    LogicInstance inst(lib, pack_dir.string());
    rumi_sensor_frame f{};
    f.size = sizeof(f);
    f.t_us = 1000000;
    f.tof_mm[0] = f.tof_mm[1] = f.tof_mm[2] = RUMI_TOF_NO_TARGET;
    rumi_command c{};
    CHECK(inst.step(f, c) == 0);
    CHECK(std::string(inst.state_name(c.state)) == "CRUISE");

    // A pack for another logic must be refused.
    const fs::path other = tmp / "packs" / "other";
    fs::remove_all(other);
    write_file(other / "params.json", read_file(pack_dir / "params.json"));
    Manifest pm = load_and_verify_manifest(pack_dir.string());
    pm.logic_name = "something_else";
    pm.files[0].sha256 = sha256_file_hex((other / "params.json").string());
    pm.files[0].bytes = fs::file_size(other / "params.json");
    write_file(other / "manifest.json", manifest_to_json(pm));
    CHECK_MSG(throws([&] { LogicInstance x(lib, other.string()); }), "pack for a different logic must be refused");

    // A tampered params.json (digest no longer matches) must be refused.
    const fs::path tampered = tmp / "packs" / "tampered";
    fs::remove_all(tampered);
    fs::copy(pack_dir, tampered, fs::copy_options::recursive);
    write_file(tampered / "params.json", read_file(pack_dir / "params.json") + " ");
    CHECK_MSG(throws([&] { LogicInstance x(lib, tampered.string()); }), "tampered pack must be refused");

    // A manifest whose version differs from the plug-in's own version must be refused.
    const fs::path d2 = tmp / "logic" / "tof_drive-wrong";
    fs::remove_all(d2);
    fs::copy(d, d2, fs::copy_options::recursive);
    m.version = "9.9.9";
    write_file(d2 / "manifest.json", manifest_to_json(m));
    CHECK_MSG(throws([&] { LogicLibrary x(d2.string()); }), "version mismatch must be refused");
}

void test_core_config() {
    CoreConfig c = parse_core_config("{}");
    CHECK(c.loop_period_ms == 50 && c.steer_in1 == 17 && c.tof_addresses[1] == 0x2D);
    c = parse_core_config("{\"loop_period_ms\": 40, \"gpio\": {\"chip\": \"/dev/gpiochip4\"}, \"i2c\": {\"tof_addresses\": [48, 49, 50]}}");
    CHECK(c.loop_period_ms == 40 && c.gpio_chip == "/dev/gpiochip4" && c.tof_addresses[0] == 48);
    CHECK(throws([] { parse_core_config("{\"unknown\": 1}"); }));
    CHECK(throws([] { parse_core_config("{\"gpio\": {\"steer_in1\": 27}}"); })); // equals steer_in2
    CHECK(throws([] { parse_core_config("{\"i2c\": {\"tof_addresses\": [41, 45, 47]}}"); })); // 0x29 reserved
    CHECK(throws([] { parse_core_config("{\"loop_period_ms\": 5}"); }));
    CHECK(throws([] { parse_core_config("{\"watchdog_ms\": 60}"); })); // < 2x period
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: test_pack <base pack dir> <libtof_drive.so>\n");
        return 2;
    }
    const fs::path pack_dir = fs::absolute(argv[1]);
    const fs::path so = fs::absolute(argv[2]);
    const fs::path tmp = fs::temp_directory_path() / "rumicar-test-pack";
    fs::create_directories(tmp);

    test_sha256_known_vector();
    test_committed_base_pack_is_consistent(pack_dir);
    test_manifest_rejections(tmp);
    test_logic_library_loads(tmp, so, pack_dir);
    test_core_config();
    return check::summary("test_pack");
}
