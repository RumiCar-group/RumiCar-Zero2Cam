// Control and telemetry servers exercised over real TCP connections on
// 127.0.0.1, with the request handler and version store behind them.
// Usage: test_network <base pack dir>
#include <arpa/inet.h>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include <nlohmann/json.hpp>

#include "car_state.hpp"
#include "check.hpp"
#include "log.hpp"
#include "net/control_server.hpp"
#include "net/socket.hpp"
#include "net/telemetry_frame.hpp"
#include "net/telemetry_server.hpp"
#include "request_handler.hpp"
#include "sha256.hpp"
#include "version_store.hpp"
#include "wifi.hpp"

using namespace rumicar;
using json = nlohmann::json;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

// --- a plain blocking TCP client ---------------------------------------------
class Client {
public:
    explicit Client(uint16_t port, int recv_timeout_ms = 3000) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        connected_ = ::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0;
        net::set_timeouts(fd_, 2000, recv_timeout_ms);
    }
    ~Client() { close(); }
    bool connected() const { return connected_; }
    void close() { if (fd_ >= 0) { ::close(fd_); fd_ = -1; } }

    void send_line(const std::string& line) {
        const std::string l = line + "\n";
        net::write_all(fd_, l.data(), l.size());
    }
    // Next line, or empty on timeout/close.
    std::string read_line() {
        while (true) {
            const size_t nl = buf_.find('\n');
            if (nl != std::string::npos) {
                std::string line = buf_.substr(0, nl);
                buf_.erase(0, nl + 1);
                return line;
            }
            char tmp[4096];
            const long n = net::read_some(fd_, tmp, sizeof tmp);
            if (n <= 0) return "";
            buf_.append(tmp, static_cast<size_t>(n));
        }
    }
    // Exactly n bytes, or fewer on timeout/close.
    std::vector<uint8_t> read_exact(size_t n) {
        std::vector<uint8_t> out;
        while (out.size() < n) {
            if (!buf_.empty()) {
                const size_t take = std::min(n - out.size(), buf_.size());
                out.insert(out.end(), buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(take));
                buf_.erase(0, take);
                continue;
            }
            char tmp[8192];
            const long got = net::read_some(fd_, tmp, sizeof tmp);
            if (got <= 0) break;
            buf_.append(tmp, static_cast<size_t>(got));
        }
        return out;
    }

private:
    int fd_ = -1;
    bool connected_ = false;
    std::string buf_;
};

long long g_id = 0;

// Sends a request and returns the matching response (events received in
// between are stored in `events`).
json request(Client& c, const std::string& type, const json& params, std::vector<json>* events = nullptr) {
    const long long id = ++g_id;
    json req = {{"id", id}, {"type", type}};
    if (!params.is_null()) req["params"] = params;
    c.send_line(req.dump());
    for (int i = 0; i < 50; ++i) {
        const std::string line = c.read_line();
        if (line.empty()) break;
        json msg = json::parse(line, nullptr, false);
        if (msg.is_discarded()) break;
        if (msg.contains("event")) {
            if (events) events->push_back(msg);
            continue;
        }
        if (msg.value("id", -1LL) == id) return msg;
    }
    return json{{"id", id}, {"ok", false}, {"error", "no response"}};
}

// Reads events until one with the given name arrives (or a timeout).
json wait_event(Client& c, const std::string& name) {
    for (int i = 0; i < 20; ++i) {
        const std::string line = c.read_line();
        if (line.empty()) break;
        json msg = json::parse(line, nullptr, false);
        if (!msg.is_discarded() && msg.value("event", "") == name) return msg;
    }
    return json();
}

std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void write_file(const fs::path& p, const std::string& content) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << content;
}

// A pack directory with a fresh manifest (same layout as make_manifest.py).
void make_pack(const fs::path& dir, const std::string& id, const std::string& version, const std::string& params,
               const json& metadata = json::object()) {
    write_file(dir / "params.json", params);
    json m = {
        {"schema", 1}, {"kind", "pack"}, {"id", id}, {"version", version}, {"created", "2026-10-18T00:00:00Z"},
        {"logic", {{"name", "tof_drive"}, {"min_abi", 1}}},
        {"files", json::array({{{"path", "params.json"}, {"sha256", sha256_hex(params)}, {"bytes", params.size()}}})},
    };
    for (auto it = metadata.begin(); it != metadata.end(); ++it) m[it.key()] = it.value();
    write_file(dir / "manifest.json", m.dump(2) + "\n");
}

// A logic directory with a manifest (the .so is a placeholder: nothing here dlopens it).
void make_logic(const fs::path& dir, const std::string& name, const std::string& version) {
    const std::string blob = "not a real shared object";
    write_file(dir / ("lib" + name + ".so"), blob);
    json m = {
        {"schema", 1}, {"kind", "logic"}, {"name", name}, {"version", version}, {"abi", 2},
        {"entry", "lib" + name + ".so"}, {"created", "2026-10-18T00:00:00Z"},
        {"files", json::array({{{"path", "lib" + name + ".so"}, {"sha256", sha256_hex(blob)}, {"bytes", blob.size()}}})},
    };
    write_file(dir / "manifest.json", m.dump(2) + "\n");
}

std::string b64encode(const std::string& in) {
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    while (i + 2 < in.size()) {
        const uint32_t v = (static_cast<uint8_t>(in[i]) << 16) | (static_cast<uint8_t>(in[i + 1]) << 8) | static_cast<uint8_t>(in[i + 2]);
        out += tbl[(v >> 18) & 63]; out += tbl[(v >> 12) & 63]; out += tbl[(v >> 6) & 63]; out += tbl[v & 63];
        i += 3;
    }
    if (i + 1 == in.size()) {
        const uint32_t v = static_cast<uint8_t>(in[i]) << 16;
        out += tbl[(v >> 18) & 63]; out += tbl[(v >> 12) & 63]; out += "==";
    } else if (i + 2 == in.size()) {
        const uint32_t v = (static_cast<uint8_t>(in[i]) << 16) | (static_cast<uint8_t>(in[i + 1]) << 8);
        out += tbl[(v >> 18) & 63]; out += tbl[(v >> 12) & 63]; out += tbl[(v >> 6) & 63]; out += '=';
    }
    return out;
}

// Declared file list for upload_begin from a directory on disk.
json file_list(const fs::path& dir) {
    json files = json::array();
    for (const auto& entry : fs::recursive_directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        const std::string rel = fs::relative(entry.path(), dir).generic_string();
        const std::string content = read_file(entry.path());
        files.push_back({{"path", rel}, {"sha256", sha256_hex(content)}, {"bytes", content.size()}});
    }
    return files;
}

// Sends every file of `dir` in chunks of `chunk` bytes; returns the first failing response (or null).
json send_files(Client& c, const fs::path& dir, size_t chunk) {
    for (const auto& entry : fs::recursive_directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        const std::string rel = fs::relative(entry.path(), dir).generic_string();
        const std::string content = read_file(entry.path());
        for (size_t off = 0; off < content.size(); off += chunk) {
            const json r = request(c, "upload_chunk", {{"path", rel}, {"offset", off}, {"data", b64encode(content.substr(off, chunk))}});
            if (!r["ok"].get<bool>()) return r;
        }
    }
    return json();
}

// Stands in for the control loop's activation handling.
struct FakeLoop {
    CarState& state;
    const VersionStore& store;
    std::atomic<bool> fail_next{false};
    std::atomic<bool> quit{false};
    std::thread thread;

    FakeLoop(CarState& s, const VersionStore& v) : state(s), store(v) {
        thread = std::thread([this] { run(); });
    }
    ~FakeLoop() {
        quit.store(true);
        thread.join();
    }
    void run() {
        while (!quit.load()) {
            if (state.take_activation_request()) {
                std::string reason;
                auto sel = store.read_current(reason);
                if (!sel) state.complete_activation(ActivationResult{false, "", "", reason});
                else if (fail_next.exchange(false)) state.complete_activation(ActivationResult{false, sel->logic_dir, sel->pack_dir, "self-test failed (simulated)"});
                else state.complete_activation(ActivationResult{true, sel->logic_dir, sel->pack_dir, ""});
            }
            std::this_thread::sleep_for(5ms);
        }
    }
};

void test_telemetry_frame_encoding() {
    const auto f = net::encode_telemetry_frame("{\"seq\":1}", {1, 2, 3});
    CHECK(f.size() == 11 + 9 + 3);
    CHECK(std::memcmp(f.data(), "RMCT", 4) == 0);
    CHECK(f[4] == 1);
    CHECK(f[5] == 9 && f[6] == 0);
    CHECK(f[7] == 3 && f[8] == 0 && f[9] == 0 && f[10] == 0);
    CHECK(f[20] == 1 && f[21] == 2 && f[22] == 3);
    bool threw = false;
    try { net::encode_telemetry_frame("", {}); } catch (const std::exception&) { threw = true; }
    CHECK(threw);
}

void test_log_ring() {
    for (int i = 0; i < 10; ++i) log::info("ring " + std::to_string(i));
    const auto r = log::recent(3);
    CHECK(r.size() == 3);
    CHECK(r.back().find("ring 9") != std::string::npos);
    CHECK(r.front().find("ring 7") != std::string::npos);
}

void test_startup_mode() {
    // The mode the core starts in comes from core.json (startup_mode); the
    // default is record_only so the car waits for the PC (docs 3.5 / 3.9).
    CarState rec(1000, DriveMode::RecordOnly);
    CHECK(rec.mode() == DriveMode::RecordOnly);
    CHECK(rec.frame_inputs(CarState::Clock::now()).mode == DriveMode::RecordOnly);
    CHECK(rec.status_json()["mode"] == "record_only" && rec.status_json()["startup_mode"] == "record_only");
    rec.set_mode(DriveMode::Auto, CarState::Clock::now());
    CHECK(rec.mode() == DriveMode::Auto && rec.status_json()["startup_mode"] == "record_only");
    CarState dflt(1000);
    CHECK(dflt.mode() == DriveMode::Auto);
    DriveMode m;
    CHECK(parse_drive_mode("record_only", m) && m == DriveMode::RecordOnly);
    CHECK(parse_drive_mode("auto", m) && m == DriveMode::Auto);
    CHECK(!parse_drive_mode("", m));
}

void test_version_store_names() {
    CHECK(VersionStore::is_plain_dir_name("base-tof-0.1.0"));
    CHECK(!VersionStore::is_plain_dir_name(""));
    CHECK(!VersionStore::is_plain_dir_name(".hidden"));
    CHECK(!VersionStore::is_plain_dir_name("../x"));
    CHECK(!VersionStore::is_plain_dir_name("a/b"));
    CHECK(!VersionStore::is_plain_dir_name("a\\b"));
}

void test_servers(const fs::path& base_pack) {
    const fs::path tmp = fs::temp_directory_path() / ("rumicar-net-" + std::to_string(::getpid()));
    fs::remove_all(tmp);
    const fs::path state_dir = tmp / "state";
    const std::string base_params = read_file(base_pack / "params.json");
    make_pack(state_dir / "packs" / "base-tof-0.1.0", "base-tof", "0.1.0", base_params);
    make_pack(state_dir / "packs" / "base-tof-0.2.0", "base-tof", "0.2.0", base_params,
              {{"description", "信号機ありケース1"}, {"capabilities", {"walls", "traffic_light"}},
               {"scenarios", {"壁のみの走行データ", "信号機ありケース1"}}, {"parent", "base-tof-0.1.0"}});
    make_pack(state_dir / "packs" / "broken-0.1.0", "broken", "0.1.0", base_params);
    write_file(state_dir / "packs" / "broken-0.1.0" / "params.json", "tampered");
    make_logic(state_dir / "logic" / "tof_drive-0.1.0", "tof_drive", "0.1.0");
    // Another logic with a pack of its own (switching needs both, docs 5.2).
    make_logic(state_dir / "logic" / "py_drive-0.1.0", "py_drive", "0.1.0");
    make_pack(state_dir / "packs" / "my-drive-20261023.101500", "my-drive", "20261023.101500", "{}",
              {{"logic", {{"name", "py_drive"}, {"min_abi", 2}}}});

    VersionStore store(state_dir.string());
    store.activate("logic", "tof_drive-0.1.0");
    store.activate("pack", "base-tof-0.1.0");
    {
        std::string reason;
        const auto sel = store.read_current(reason);
        CHECK_MSG(sel.has_value(), reason);
    }
    CHECK(store.link_target("current", "pack") == "base-tof-0.1.0");
    CHECK(store.link_target("previous", "pack").empty());

    CarState state(300);   // short heartbeat timeout for the test
    state.set_max_drive(800);
    WifiAp wifi("/usr/bin/nmcli", "");   // no access point profile on the test host
    RequestHandler handler(state, store, wifi, "TestCar", "9.9.9", 2000ms);
    std::atomic<int> connects{0}, disconnects{0};
    net::ControlServer control("127.0.0.1", 0,
        [&](const std::string& type, const json& params) { return handler.handle(type, params); },
        [&](bool connected, const std::string&) {
            if (connected) ++connects;
            else { ++disconnects; handler.on_client_disconnected(); }
        });
    net::TelemetryServer telemetry("127.0.0.1", 0, 2);
    state.set_event_sink([&](const std::string& event, const json& data) { control.push_event(event, data); });
    control.start();
    telemetry.start();
    CHECK(control.port() != 0);
    CHECK(telemetry.port() != 0);
    FakeLoop loop(state, store);

    // --- hello and status ----------------------------------------------------
    Client c(control.port());
    CHECK(c.connected());
    json r = request(c, "get_status", nullptr);
    CHECK(!r["ok"].get<bool>());
    CHECK(r["error"].get<std::string>().find("hello") != std::string::npos);
    r = request(c, "hello", {{"client", "test"}, {"protocol", 2}});
    CHECK(!r["ok"].get<bool>());
    r = request(c, "hello", {{"client", "test"}, {"protocol", 1}});
    CHECK(r["ok"].get<bool>());
    CHECK(r["result"]["car"] == "TestCar");
    CHECK(r["result"]["core_version"] == "9.9.9");
    CHECK(r["result"]["protocol"] == 1);
    for (int i = 0; i < 100 && connects.load() == 0; ++i) std::this_thread::sleep_for(10ms);
    CHECK(connects.load() == 1);
    CHECK(control.client_connected());

    RunningStatus running;
    running.logic_name = "tof_drive";
    running.logic_version = "0.1.0";
    running.pack_id = "base-tof";
    running.pack_version = "0.1.0";
    running.pack_description = "壁のみ";
    running.pack_capabilities = {"walls"};
    running.state_name = "CRUISE";
    state.update_running(running);
    r = request(c, "get_status", nullptr);
    CHECK(r["ok"].get<bool>());
    CHECK(r["result"]["mode"] == "auto");
    CHECK(r["result"]["startup_mode"] == "auto");
    CHECK(r["result"]["estop"] == false);
    CHECK(r["result"]["logic_version"] == "0.1.0");
    CHECK(r["result"]["state_name"] == "CRUISE");
    CHECK(r["result"]["pack_description"] == "壁のみ");
    CHECK(r["result"]["pack_capabilities"] == json::array({"walls"}));
    CHECK(r["result"]["pack_scenarios"] == json::array());
    CHECK(r["result"]["throttled"] == 0);
    // Power flags from the firmware are reported in the status (docs 3.9).
    CHECK(state.update_throttled(0x50001u));
    CHECK(!state.update_throttled(0x50001u));
    r = request(c, "get_status", nullptr);
    CHECK(r["result"]["throttled"] == 0x50001u);
    CHECK(state.update_throttled(0));

    // --- modes, manual drive, heartbeat -----------------------------------------
    r = request(c, "manual_drive", {{"steer", 1}, {"drive_permille", 500}});
    CHECK(!r["ok"].get<bool>());   // not in manual mode
    r = request(c, "set_mode", {{"mode", "sideways"}});
    CHECK(!r["ok"].get<bool>());
    std::vector<json> events;
    r = request(c, "set_mode", {{"mode", "manual"}}, &events);
    CHECK(r["ok"].get<bool>());
    json ev = wait_event(c, "status");
    CHECK(!ev.is_null() && ev["data"]["mode"] == "manual");
    r = request(c, "manual_drive", {{"steer", 1}, {"drive_permille", 500}});
    CHECK(r["ok"].get<bool>());
    auto in = state.frame_inputs(CarState::Clock::now());
    CHECK(in.mode == DriveMode::Manual);
    CHECK(in.manual_steer == 1 && in.manual_drive == 500);
    CHECK(!in.manual_timed_out);
    r = request(c, "manual_drive", {{"steer", 2}, {"drive_permille", 0}});
    CHECK(!r["ok"].get<bool>());
    r = request(c, "manual_drive", {{"steer", 0}, {"drive_permille", 1001}});
    CHECK(!r["ok"].get<bool>());
    std::this_thread::sleep_for(400ms);   // longer than the 300 ms heartbeat timeout
    in = state.frame_inputs(CarState::Clock::now());
    CHECK(in.manual_timed_out);
    r = request(c, "heartbeat", nullptr);
    CHECK(r["ok"].get<bool>());
    in = state.frame_inputs(CarState::Clock::now());
    CHECK(!in.manual_timed_out);

    // --- emergency stop ---------------------------------------------------------
    r = request(c, "estop", nullptr);
    CHECK(r["ok"].get<bool>());
    ev = wait_event(c, "estop");
    CHECK(!ev.is_null() && ev["data"]["active"] == true && ev["data"]["reason"] == "request");
    in = state.frame_inputs(CarState::Clock::now());
    CHECK(in.estop);
    CHECK(in.manual_drive == 0);   // the manual command is cleared by an e-stop
    r = request(c, "get_status", nullptr);
    CHECK(r["result"]["estop"] == true && r["result"]["estop_reason"] == "request");
    r = request(c, "estop_clear", nullptr);
    CHECK(r["ok"].get<bool>());
    ev = wait_event(c, "estop");
    CHECK(!ev.is_null() && ev["data"]["active"] == false);
    CHECK(!state.estop());
    r = request(c, "set_mode", {{"mode", "auto"}});
    CHECK(r["ok"].get<bool>());
    CHECK(state.mode() == DriveMode::Auto);

    // --- auto limit (docs 5.2): within max_drive_permille, reported in the status ---------------
    r = request(c, "get_status", nullptr);
    CHECK(r["result"]["auto_limit"] == 800 && r["result"]["max_drive_permille"] == 800);
    r = request(c, "set_auto_limit", {{"drive_permille", 450}}, &events);
    CHECK(r["ok"].get<bool>() && r["result"]["auto_limit"] == 450);
    ev = wait_event(c, "status");
    CHECK(!ev.is_null() && ev["data"]["auto_limit"] == 450);
    CHECK(state.frame_inputs(CarState::Clock::now()).auto_limit == 450);
    r = request(c, "set_auto_limit", {{"drive_permille", 1000}});
    CHECK(r["ok"].get<bool>() && r["result"]["auto_limit"] == 800);   // clamped to the maximum
    r = request(c, "set_auto_limit", {{"drive_permille", -1}});
    CHECK(!r["ok"].get<bool>());
    r = request(c, "set_auto_limit", nullptr);
    CHECK(!r["ok"].get<bool>());

    // --- versions: list / activate / rollback / failure --------------------------
    r = request(c, "list_versions", nullptr);
    CHECK(r["ok"].get<bool>());
    CHECK(r["result"]["logic"].is_array() && r["result"]["logic"].size() == 2);
    {
        bool saw_tof = false;
        for (const auto& l : r["result"]["logic"]) saw_tof = saw_tof || (l["name"] == "tof_drive-0.1.0" && l["verified"] == true);
        CHECK(saw_tof);
        bool saw_pack_logic = false;
        for (const auto& p : r["result"]["packs"])
            if (p["name"] == "my-drive-20261023.101500") saw_pack_logic = p["logic"] == "py_drive";
        CHECK_MSG(saw_pack_logic, "list_versions must say which logic a pack is for");
    }
    CHECK(r["result"]["current"]["pack"] == "base-tof-0.1.0");
    CHECK(r["result"]["current"]["logic"] == "tof_drive-0.1.0");
    {
        bool saw_ok = false, saw_broken = false, saw_meta = false;
        for (const auto& p : r["result"]["packs"]) {
            if (p["name"] == "base-tof-0.1.0") saw_ok = p["verified"].get<bool>() && p["version"] == "0.1.0" && p["description"] == "" && p["capabilities"] == json::array();
            if (p["name"] == "broken-0.1.0") saw_broken = !p["verified"].get<bool>() && !p["error"].get<std::string>().empty();
            if (p["name"] == "base-tof-0.2.0")
                saw_meta = p["description"] == "信号機ありケース1" && p["capabilities"] == json::array({"walls", "traffic_light"}) &&
                           p["scenarios"].size() == 2 && p["parent"] == "base-tof-0.1.0";
        }
        CHECK(saw_ok);
        CHECK(saw_broken);
        CHECK_MSG(saw_meta, "list_versions must carry the manifest metadata");
    }
    r = request(c, "activate", {{"kind", "pack"}, {"name", "base-tof-0.2.0"}});
    CHECK_MSG(r["ok"].get<bool>(), r.dump());
    CHECK(r["ok"].get<bool>() && r["result"].value("pack_dir", "").find("base-tof-0.2.0") != std::string::npos);
    CHECK(store.link_target("current", "pack") == "base-tof-0.2.0");
    CHECK(store.link_target("previous", "pack") == "base-tof-0.1.0");
    ev = wait_event(c, "activation");
    CHECK(!ev.is_null() && ev["data"]["ok"] == true);
    r = request(c, "activate", {{"kind", "pack"}, {"name", "nope-1.0.0"}});
    CHECK(!r["ok"].get<bool>());
    r = request(c, "activate", {{"kind", "pack"}, {"name", "../etc"}});
    CHECK(!r["ok"].get<bool>());
    r = request(c, "activate", {{"kind", "firmware"}, {"name", "x"}});
    CHECK(!r["ok"].get<bool>());
    CHECK(store.link_target("current", "pack") == "base-tof-0.2.0");   // untouched by the rejected requests
    r = request(c, "rollback", {{"kind", "pack"}});
    CHECK_MSG(r["ok"].get<bool>(), r.dump());
    CHECK(store.link_target("current", "pack") == "base-tof-0.1.0");
    CHECK(store.link_target("previous", "pack") == "base-tof-0.2.0");
    r = request(c, "rollback", {{"kind", "logic"}});
    CHECK(!r["ok"].get<bool>());   // no previous logic
    loop.fail_next.store(true);
    r = request(c, "activate", {{"kind", "pack"}, {"name", "base-tof-0.2.0"}});
    CHECK(!r["ok"].get<bool>());
    CHECK(r["error"].get<std::string>().find("simulated") != std::string::npos);

    // restore_current: back to what is running, without touching previous
    store.restore_current("pack", (state_dir / "packs" / "base-tof-0.1.0").string());
    CHECK(store.link_target("current", "pack") == "base-tof-0.1.0");
    CHECK(store.link_target("previous", "pack") == "base-tof-0.1.0");
    {
        bool threw = false;
        try { store.restore_current("pack", (tmp / "elsewhere").string()); } catch (const std::exception&) { threw = true; }
        CHECK(threw);
    }

    // --- a logic and its pack together (activate_pair), and back (rollback both) ---------------
    r = request(c, "activate_pair", {{"logic", "py_drive-0.1.0"}, {"pack", "nope-1"}});
    CHECK(!r["ok"].get<bool>());
    CHECK(store.link_target("current", "logic") == "tof_drive-0.1.0");   // nothing written when either is wrong
    r = request(c, "activate_pair", {{"logic", "../x"}, {"pack", "my-drive-20261023.101500"}});
    CHECK(!r["ok"].get<bool>());
    r = request(c, "activate_pair", {{"logic", "py_drive-0.1.0"}});
    CHECK(!r["ok"].get<bool>());
    r = request(c, "activate_pair", {{"logic", "py_drive-0.1.0"}, {"pack", "my-drive-20261023.101500"}});
    CHECK_MSG(r["ok"].get<bool>(), r.dump());
    CHECK(store.link_target("current", "logic") == "py_drive-0.1.0" && store.link_target("current", "pack") == "my-drive-20261023.101500");
    CHECK(store.link_target("previous", "logic") == "tof_drive-0.1.0" && store.link_target("previous", "pack") == "base-tof-0.1.0");
    r = request(c, "rollback", {{"kind", "both"}});
    CHECK_MSG(r["ok"].get<bool>(), r.dump());
    CHECK(store.link_target("current", "logic") == "tof_drive-0.1.0" && store.link_target("current", "pack") == "base-tof-0.1.0");
    CHECK(store.link_target("previous", "logic") == "py_drive-0.1.0");
    r = request(c, "rollback", {{"kind", "everything"}});
    CHECK(!r["ok"].get<bool>());
    // The pair is never seen half-written: a reader racing with activate_pair sees the old or the new pair.
    {
        std::atomic<bool> stop{false};
        std::atomic<int> mixed{0}, reads{0};
        std::thread reader([&] {
            while (!stop.load()) {
                std::string reason;
                if (const auto sel = store.read_current(reason)) {
                    const bool py_logic = sel->logic_dir.find("py_drive") != std::string::npos;
                    const bool py_pack = sel->pack_dir.find("my-drive") != std::string::npos;
                    if (py_logic != py_pack) ++mixed;
                    ++reads;
                }
            }
        });
        for (int i = 0; i < 200; ++i) {
            store.activate_pair("py_drive-0.1.0", "my-drive-20261023.101500");
            store.activate_pair("tof_drive-0.1.0", "base-tof-0.1.0");
        }
        stop.store(true);
        reader.join();
        CHECK(reads.load() > 0);
        CHECK_MSG(mixed.load() == 0, std::to_string(mixed.load()) + " half-written pairs seen");
    }

    // --- uploads from the PC -----------------------------------------------------
    {
        const fs::path src = tmp / "src-0.3.0";
        make_pack(src, "base-tof", "0.3.0", base_params + "\n\n\n");   // different content than 0.1.0
        write_file(src / "notes" / "readme.txt", std::string(100000, 'x'));           // a sub-directory and a multi-chunk file
        {   // the manifest must list every file: rewrite it with the extra file
            json m = json::parse(read_file(src / "manifest.json"));
            const std::string notes = read_file(src / "notes" / "readme.txt");
            m["files"].push_back({{"path", "notes/readme.txt"}, {"sha256", sha256_hex(notes)}, {"bytes", notes.size()}});
            write_file(src / "manifest.json", m.dump(2) + "\n");
        }
        r = request(c, "upload_begin", {{"kind", "pack"}, {"name", "base-tof-0.3.0"}, {"files", file_list(src)}});
        CHECK_MSG(r["ok"].get<bool>(), r.dump());
        CHECK(r["result"]["chunk_bytes"] == 32768);
        r = request(c, "upload_begin", {{"kind", "pack"}, {"name", "other-1.0.0"}, {"files", file_list(src)}});
        CHECK(!r["ok"].get<bool>());   // one at a time
        r = request(c, "upload_chunk", {{"path", "params.json"}, {"offset", 5}, {"data", "QUJD"}});
        CHECK(!r["ok"].get<bool>());   // wrong offset
        r = request(c, "upload_chunk", {{"path", "undeclared.bin"}, {"offset", 0}, {"data", "QUJD"}});
        CHECK(!r["ok"].get<bool>());
        r = request(c, "upload_chunk", {{"path", "params.json"}, {"offset", 0}, {"data", "not base64!"}});
        CHECK(!r["ok"].get<bool>());
        r = request(c, "upload_end", nullptr);
        CHECK(!r["ok"].get<bool>());   // incomplete: nothing installed, upload discarded
        CHECK(!fs::exists(state_dir / "packs" / "base-tof-0.3.0"));
        CHECK(!fs::exists(state_dir / "incoming" / "pack-base-tof-0.3.0"));

        // The whole thing, in 1000-byte chunks.
        r = request(c, "upload_begin", {{"kind", "pack"}, {"name", "base-tof-0.3.0"}, {"files", file_list(src)}});
        CHECK_MSG(r["ok"].get<bool>(), r.dump());
        json fail = send_files(c, src, 1000);
        CHECK_MSG(fail.is_null(), fail.dump());
        r = request(c, "upload_end", nullptr);
        CHECK_MSG(r["ok"].get<bool>(), r.dump());
        CHECK(r["result"]["version"] == "0.3.0" && r["result"]["id"] == "base-tof");
        CHECK(fs::is_regular_file(state_dir / "packs" / "base-tof-0.3.0" / "notes" / "readme.txt"));
        r = request(c, "list_versions", nullptr);
        {
            bool found = false;
            for (const auto& p : r["result"]["packs"]) if (p["name"] == "base-tof-0.3.0") found = p["verified"].get<bool>();
            CHECK(found);
        }
        r = request(c, "upload_begin", {{"kind", "pack"}, {"name", "base-tof-0.3.0"}, {"files", file_list(src)}});
        CHECK(!r["ok"].get<bool>() && r["error"].get<std::string>().find("already installed") != std::string::npos);

        // Rejections at upload_begin.
        json bad = file_list(src);
        bad[0]["path"] = "../escape";
        r = request(c, "upload_begin", {{"kind", "pack"}, {"name", "x-1.0.0"}, {"files", bad}});
        CHECK(!r["ok"].get<bool>());
        json no_manifest = json::array({{{"path", "params.json"}, {"sha256", std::string(64, 'a')}, {"bytes", 1}}});
        r = request(c, "upload_begin", {{"kind", "pack"}, {"name", "x-1.0.0"}, {"files", no_manifest}});
        CHECK(!r["ok"].get<bool>());
        r = request(c, "upload_begin", {{"kind", "pack"}, {"name", "../x"}, {"files", file_list(src)}});
        CHECK(!r["ok"].get<bool>());
        r = request(c, "upload_begin", {{"kind", "firmware"}, {"name", "x-1.0.0"}, {"files", file_list(src)}});
        CHECK(!r["ok"].get<bool>());

        // Content that does not match its declared digest is refused at upload_end.
        const fs::path bad_src = tmp / "src-bad";
        make_pack(bad_src, "base-tof", "0.4.0", base_params + " ");
        json declared = file_list(bad_src);
        write_file(bad_src / "params.json", base_params + "!");   // changed after declaring (same length, other digest)
        r = request(c, "upload_begin", {{"kind", "pack"}, {"name", "base-tof-0.4.0"}, {"files", declared}});
        CHECK(r["ok"].get<bool>());
        fail = send_files(c, bad_src, 32768);
        CHECK(fail.is_null());
        r = request(c, "upload_end", nullptr);
        CHECK(!r["ok"].get<bool>() && r["error"].get<std::string>().find("sha256") != std::string::npos);
        CHECK(!fs::exists(state_dir / "packs" / "base-tof-0.4.0"));

        // A directory name that does not match the manifest is refused at upload_end.
        r = request(c, "upload_begin", {{"kind", "pack"}, {"name", "base-tof-9.9.9"}, {"files", file_list(src)}});
        CHECK(r["ok"].get<bool>());
        fail = send_files(c, src, 32768);
        CHECK(fail.is_null());
        r = request(c, "upload_end", nullptr);
        CHECK(!r["ok"].get<bool>() && r["error"].get<std::string>().find("does not match") != std::string::npos);

        // abort cleans up and allows a new upload.
        r = request(c, "upload_begin", {{"kind", "pack"}, {"name", "base-tof-0.5.0"}, {"files", file_list(src)}});
        CHECK(r["ok"].get<bool>());
        r = request(c, "upload_abort", nullptr);
        CHECK(r["ok"].get<bool>());
        CHECK(!fs::exists(state_dir / "incoming" / "pack-base-tof-0.5.0"));
        r = request(c, "upload_end", nullptr);
        CHECK(!r["ok"].get<bool>());

        // delete_version: never the current or previous one.
        r = request(c, "delete_version", {{"kind", "pack"}, {"name", "base-tof-0.1.0"}});
        CHECK(!r["ok"].get<bool>() && r["error"].get<std::string>().find("current") != std::string::npos);
        r = request(c, "delete_version", {{"kind", "pack"}, {"name", "base-tof-0.3.0"}});
        CHECK_MSG(r["ok"].get<bool>(), r.dump());
        CHECK(!fs::exists(state_dir / "packs" / "base-tof-0.3.0"));
        r = request(c, "delete_version", {{"kind", "pack"}, {"name", "base-tof-0.3.0"}});
        CHECK(!r["ok"].get<bool>());
        r = request(c, "delete_version", {{"kind", "pack"}, {"name", "../x"}});
        CHECK(!r["ok"].get<bool>());
    }

    // --- misc requests -----------------------------------------------------------
    r = request(c, "set_wifi", {{"ssid", "X"}, {"channel", 6}});
    CHECK(!r["ok"].get<bool>() && r["error"].get<std::string>().find("not configured") != std::string::npos);
    r = request(c, "set_wifi", {{"ssid", "X"}, {"channel", 14}});
    CHECK(!r["ok"].get<bool>());
    r = request(c, "get_log", {{"lines", 3}});
    CHECK(r["ok"].get<bool>() && r["result"]["lines"].is_array() && r["result"]["lines"].size() == 3);
    r = request(c, "get_log", {{"lines", 0}});
    CHECK(!r["ok"].get<bool>());
    r = request(c, "bogus", nullptr);
    CHECK(!r["ok"].get<bool>());
    c.send_line("this is not json");
    {
        const json m = json::parse(c.read_line());
        CHECK(m["id"] == 0 && m["ok"] == false);
    }
    c.send_line("[1,2]");
    {
        const json m = json::parse(c.read_line());
        CHECK(m["id"] == 0 && m["ok"] == false);
    }
    c.send_line("{\"id\":\"x\",\"type\":\"heartbeat\"}");
    {
        const json m = json::parse(c.read_line());
        CHECK(m["id"] == 0 && m["ok"] == false);
    }

    // --- a second control client is refused -------------------------------------
    {
        Client second(control.port());
        CHECK(second.connected());
        const std::string line = second.read_line();
        CHECK(!line.empty());
        const json m = json::parse(line, nullptr, false);
        CHECK(!m.is_discarded() && m["ok"] == false && m["error"].get<std::string>().find("busy") != std::string::npos);
        CHECK(second.read_line().empty());   // closed by the server
    }
    r = request(c, "heartbeat", nullptr);   // the first client still works
    CHECK(r["ok"].get<bool>());

    // --- telemetry ---------------------------------------------------------------
    Client t(telemetry.port());
    CHECK(t.connected());
    for (int i = 0; i < 100 && telemetry.client_count() == 0; ++i) std::this_thread::sleep_for(10ms);
    CHECK(telemetry.client_count() == 1);
    // One frame at a time: the test server keeps only 2 frames per client, so
    // publishing several before the reader catches up would (correctly) drop some.
    for (uint32_t seq = 1; seq <= 3; ++seq) {
        const json header = {{"seq", seq}, {"tof_mm", {100, 200, 300}}, {"state_name", "CRUISE"}};
        telemetry.publish(std::make_shared<const std::vector<uint8_t>>(net::encode_telemetry_frame(header.dump(), {})));
        const auto fixed = t.read_exact(11);
        CHECK(fixed.size() == 11);
        if (fixed.size() != 11) break;
        CHECK(std::memcmp(fixed.data(), "RMCT", 4) == 0 && fixed[4] == 1);
        const size_t hlen = fixed[5] | (static_cast<size_t>(fixed[6]) << 8);
        const size_t plen = fixed[7] | (static_cast<size_t>(fixed[8]) << 8) | (static_cast<size_t>(fixed[9]) << 16) | (static_cast<size_t>(fixed[10]) << 24);
        CHECK(plen == 0);
        const auto body = t.read_exact(hlen);
        CHECK(body.size() == hlen);
        const json h = json::parse(body.begin(), body.end(), nullptr, false);
        CHECK(!h.is_discarded() && h["seq"] == seq);
    }

    // A client that never reads loses frames instead of stalling the publisher.
    {
        Client sleeper(telemetry.port());
        CHECK(sleeper.connected());
        for (int i = 0; i < 100 && telemetry.client_count() < 2; ++i) std::this_thread::sleep_for(10ms);
        const std::vector<uint8_t> big(64 * 1024, 0xAB);
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 300; ++i) {
            telemetry.publish(std::make_shared<const std::vector<uint8_t>>(net::encode_telemetry_frame("{\"seq\":0}", big)));
        }
        const auto elapsed = std::chrono::steady_clock::now() - t0;
        CHECK_MSG(elapsed < 2s, "publish must not block on a slow client");
        CHECK(telemetry.dropped_frames() > 0);
        sleeper.close();
        for (int i = 0; i < 500 && telemetry.client_count() > 1; ++i) std::this_thread::sleep_for(10ms);
        CHECK(telemetry.client_count() == 1);
        t.close();
        for (int i = 0; i < 500 && telemetry.client_count() > 0; ++i) std::this_thread::sleep_for(10ms);
        CHECK(telemetry.client_count() == 0);
    }

    // --- disconnect and reconnect ----------------------------------------------
    r = request(c, "set_mode", {{"mode", "manual"}});
    CHECK(r["ok"].get<bool>());
    r = request(c, "upload_begin", {{"kind", "pack"}, {"name", "base-tof-0.6.0"}, {"files", file_list(tmp / "src-0.3.0")}});
    CHECK(r["ok"].get<bool>());
    c.close();
    for (int i = 0; i < 200 && disconnects.load() == 0; ++i) std::this_thread::sleep_for(10ms);
    CHECK(disconnects.load() == 1);
    CHECK(!control.client_connected());
    {
        Client again(control.port());
        CHECK(again.connected());
        r = request(again, "hello", {{"client", "test2"}, {"protocol", 1}});
        CHECK(r["ok"].get<bool>());
        r = request(again, "get_status", nullptr);
        CHECK(r["ok"].get<bool>() && r["result"]["mode"] == "manual");
        CHECK(!fs::exists(state_dir / "incoming" / "pack-base-tof-0.6.0"));   // discarded on disconnect
        r = request(again, "upload_begin", {{"kind", "pack"}, {"name", "base-tof-0.6.0"}, {"files", file_list(tmp / "src-0.3.0")}});
        CHECK(r["ok"].get<bool>());
        r = request(again, "upload_abort", nullptr);
        CHECK(r["ok"].get<bool>());
    }
    for (int i = 0; i < 200 && disconnects.load() < 2; ++i) std::this_thread::sleep_for(10ms);
    CHECK(disconnects.load() == 2);

    control.stop();
    telemetry.stop();
    fs::remove_all(tmp);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: test_network <base pack dir>\n");
        return 2;
    }
    test_telemetry_frame_encoding();
    test_log_ring();
    test_startup_mode();
    test_version_store_names();
    test_servers(argv[1]);
    return check::summary("test_network");
}
