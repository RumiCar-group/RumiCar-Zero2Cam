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
    CHECK(m.logic_min_abi >= 1 && m.logic_min_abi <= RUMI_LOGIC_ABI_VERSION);   // packs made for ABI 1 stay valid
    CHECK(m.files.size() == 1 && m.files[0].path == "params.json");
    // The shipped pack says what it is (shown by the PC, docs 4.1).
    CHECK(!m.description.empty());
    CHECK(m.capabilities.size() == 1 && m.capabilities[0] == "walls");
    CHECK(m.scenarios.empty() && m.parent.empty());
}

void test_manifest_metadata() {
    // Optional metadata round-trips and is validated.
    Manifest m;
    m.kind = Manifest::Kind::Pack;
    m.id = "cam-signal";
    m.version = "0.2.0";
    m.logic_name = "cam_drive";
    m.logic_min_abi = 1;
    m.description = "信号機ありケース1：壁のみ 0.1.1 に赤・青の信号の検出を追加";
    m.capabilities = {"walls", "traffic_light"};
    m.scenarios = {"壁のみの走行データ", "信号機ありケース1"};
    m.parent = "base-tof-0.1.1";
    m.files.push_back({"vision.bin", std::string(64, 'a'), 10});
    const std::string text = manifest_to_json(m);
    Manifest back = parse_manifest(text);
    CHECK(back.description == m.description);
    CHECK(back.capabilities == m.capabilities);
    CHECK(back.scenarios == m.scenarios);
    CHECK(back.parent == "base-tof-0.1.1");

    // Absent metadata is simply empty (older manifests stay valid).
    Manifest plain = parse_manifest("{\"schema\":1,\"kind\":\"pack\",\"id\":\"x\",\"version\":\"1\",\"logic\":{\"name\":\"l\",\"min_abi\":1},"
                                    "\"files\":[{\"path\":\"p\",\"sha256\":\"" + std::string(64, 'b') + "\",\"bytes\":1}]}");
    CHECK(plain.description.empty() && plain.capabilities.empty() && plain.scenarios.empty() && plain.parent.empty());

    const std::string head = "{\"schema\":1,\"kind\":\"pack\",\"id\":\"x\",\"version\":\"1\",\"logic\":{\"name\":\"l\",\"min_abi\":1},"
                             "\"files\":[{\"path\":\"p\",\"sha256\":\"" + std::string(64, 'b') + "\",\"bytes\":1}],";
    CHECK_MSG(throws([&] { parse_manifest(head + "\"description\":\"" + std::string(kManifestMaxDescription + 1, 'd') + "\"}"); }),
              "over-long description must be rejected");
    CHECK_MSG(throws([&] { parse_manifest(head + "\"description\":5}"); }), "non-string description must be rejected");
    CHECK_MSG(throws([&] { parse_manifest(head + "\"capabilities\":\"walls\"}"); }), "capabilities must be an array");
    CHECK_MSG(throws([&] { parse_manifest(head + "\"capabilities\":[\"\"]}"); }), "empty capability must be rejected");
    CHECK_MSG(throws([&] { parse_manifest(head + "\"scenarios\":[1]}"); }), "non-string scenario must be rejected");
    CHECK(!throws([&] { parse_manifest(head + "\"description\":null,\"capabilities\":[]}"); }));
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
    m.version = TOF_DRIVE_VERSION;   // must match what the freshly built plug-in reports
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

// The camera logic plug-in loads with the ToF-only cam-base pack, reports its
// state names, accepts the base pack's structure, and refuses a tof_drive pack.
void test_cam_drive_library_loads(const fs::path& tmp, const fs::path& so, const fs::path& cam_pack_dir, const fs::path& tof_pack_dir) {
    const fs::path d = tmp / "logic" / "cam_drive-test";
    fs::remove_all(d);
    fs::create_directories(d);
    fs::copy_file(so, d / "libcam_drive.so", fs::copy_options::overwrite_existing);
    Manifest m;
    m.kind = Manifest::Kind::Logic;
    m.id = "cam_drive";
    m.version = CAM_DRIVE_VERSION;
    m.abi = RUMI_LOGIC_ABI_VERSION;
    m.entry = "libcam_drive.so";
    m.files.push_back({"libcam_drive.so", sha256_file_hex((d / "libcam_drive.so").string()), fs::file_size(d / "libcam_drive.so")});
    write_file(d / "manifest.json", manifest_to_json(m));

    auto lib = std::make_shared<LogicLibrary>(d.string());
    CHECK(std::string(lib->api().name) == "cam_drive");
    const Manifest cm = load_and_verify_manifest(cam_pack_dir.string());
    CHECK(cm.id == "cam-base" && cm.logic_name == "cam_drive" && cm.logic_min_abi == 2);
    CHECK(cm.capabilities.size() == 1 && cm.capabilities[0] == "walls");
    LogicInstance inst(lib, cam_pack_dir.string());
    CHECK(inst.aux_names().empty());   // no vision.bin: nothing to show
    rumi_sensor_frame f{};
    f.size = sizeof(f);
    f.t_us = 1000000;
    f.tof_mm[0] = f.tof_mm[1] = f.tof_mm[2] = RUMI_TOF_NO_TARGET;
    rumi_command c{};
    CHECK(inst.step(f, c) == 0);
    CHECK(std::string(inst.state_name(c.state)) == "CRUISE");
    CHECK(std::string(inst.state_name(17)) == "SIGNAL_STOP");
    CHECK(c.aux_len == 0);
    // The tof_drive pack is for another logic.
    CHECK_MSG(throws([&] { LogicInstance x(lib, tof_pack_dir.string()); }), "a tof_drive pack must be refused by cam_drive");
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

    // Start-up mode, camera on demand, access-point start and power keys (docs 3.9).
    CHECK(c.startup_mode == "record_only" && c.camera_on_demand && c.ap_start == "core" && c.wifi_txpower_dbm == 0);
    CHECK(c.throttled_path == "/sys/devices/platform/soc/soc:firmware/get_throttled" && c.power_poll_ms == 1000);
    c = parse_core_config("{\"startup_mode\": \"auto\", \"camera\": {\"on_demand\": false},"
                          " \"network\": {\"ap_start\": \"boot\", \"wifi_txpower_dbm\": 12, \"wifi_device\": \"wlan1\"},"
                          " \"power\": {\"throttled_path\": \"\", \"poll_ms\": 500}}");
    CHECK(c.startup_mode == "auto" && !c.camera_on_demand && c.ap_start == "boot" && c.wifi_txpower_dbm == 12);
    CHECK(c.wifi_device == "wlan1" && c.throttled_path.empty() && c.power_poll_ms == 500);
    CHECK(throws([] { parse_core_config("{\"startup_mode\": \"manual\"}"); }));   // manual needs the PC
    CHECK(throws([] { parse_core_config("{\"network\": {\"ap_start\": \"later\"}}"); }));
    CHECK(throws([] { parse_core_config("{\"network\": {\"wifi_txpower_dbm\": 31}}"); }));
    CHECK(throws([] { parse_core_config("{\"power\": {\"unknown\": 1}}"); }));
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 5) {
        std::fprintf(stderr, "usage: test_pack <base pack dir> <libtof_drive.so> <cam-base pack dir> <libcam_drive.so>\n");
        return 2;
    }
    const fs::path pack_dir = fs::absolute(argv[1]);
    const fs::path so = fs::absolute(argv[2]);
    const fs::path cam_pack_dir = fs::absolute(argv[3]);
    const fs::path cam_so = fs::absolute(argv[4]);
    const fs::path tmp = fs::temp_directory_path() / "rumicar-test-pack";
    fs::create_directories(tmp);

    test_sha256_known_vector();
    test_committed_base_pack_is_consistent(pack_dir);
    test_manifest_rejections(tmp);
    test_manifest_metadata();
    test_logic_library_loads(tmp, so, pack_dir);
    test_cam_drive_library_loads(tmp, cam_so, cam_pack_dir, pack_dir);
    test_core_config();
    return check::summary("test_pack");
}
