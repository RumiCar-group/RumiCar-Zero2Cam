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
void make_pack(const fs::path& dir, const std::string& id, const std::string& version, const std::string& params) {
    write_file(dir / "params.json", params);
    json m = {
        {"schema", 1}, {"kind", "pack"}, {"id", id}, {"version", version}, {"created", "2026-10-18T00:00:00Z"},
        {"logic", {{"name", "tof_drive"}, {"min_abi", 1}}},
        {"files", json::array({{{"path", "params.json"}, {"sha256", sha256_hex(params)}, {"bytes", params.size()}}})},
    };
    write_file(dir / "manifest.json", m.dump(2) + "\n");
}

// A logic directory with a manifest (the .so is a placeholder: nothing here dlopens it).
void make_logic(const fs::path& dir, const std::string& name, const std::string& version) {
    const std::string blob = "not a real shared object";
    write_file(dir / ("lib" + name + ".so"), blob);
    json m = {
        {"schema", 1}, {"kind", "logic"}, {"name", name}, {"version", version}, {"abi", 1},
        {"entry", "lib" + name + ".so"}, {"created", "2026-10-18T00:00:00Z"},
        {"files", json::array({{{"path", "lib" + name + ".so"}, {"sha256", sha256_hex(blob)}, {"bytes", blob.size()}}})},
    };
    write_file(dir / "manifest.json", m.dump(2) + "\n");
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
    make_pack(state_dir / "packs" / "base-tof-0.2.0", "base-tof", "0.2.0", base_params);
    make_pack(state_dir / "packs" / "broken-0.1.0", "broken", "0.1.0", base_params);
    write_file(state_dir / "packs" / "broken-0.1.0" / "params.json", "tampered");
    make_logic(state_dir / "logic" / "tof_drive-0.1.0", "tof_drive", "0.1.0");

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
    RequestHandler handler(state, store, "TestCar", "9.9.9", 2000ms);
    std::atomic<int> connects{0}, disconnects{0};
    net::ControlServer control("127.0.0.1", 0,
        [&](const std::string& type, const json& params) { return handler.handle(type, params); },
        [&](bool connected, const std::string&) { if (connected) ++connects; else ++disconnects; });
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
    running.state_name = "CRUISE";
    state.update_running(running);
    r = request(c, "get_status", nullptr);
    CHECK(r["ok"].get<bool>());
    CHECK(r["result"]["mode"] == "auto");
    CHECK(r["result"]["estop"] == false);
    CHECK(r["result"]["logic_version"] == "0.1.0");
    CHECK(r["result"]["state_name"] == "CRUISE");

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

    // --- versions: list / activate / rollback / failure --------------------------
    r = request(c, "list_versions", nullptr);
    CHECK(r["ok"].get<bool>());
    CHECK(r["result"]["logic"].is_array() && r["result"]["logic"].size() == 1);
    CHECK(r["result"]["logic"][0]["name"] == "tof_drive-0.1.0" && r["result"]["logic"][0]["verified"] == true);
    CHECK(r["result"]["current"]["pack"] == "base-tof-0.1.0");
    CHECK(r["result"]["current"]["logic"] == "tof_drive-0.1.0");
    {
        bool saw_ok = false, saw_broken = false;
        for (const auto& p : r["result"]["packs"]) {
            if (p["name"] == "base-tof-0.1.0") saw_ok = p["verified"].get<bool>() && p["version"] == "0.1.0";
            if (p["name"] == "broken-0.1.0") saw_broken = !p["verified"].get<bool>() && !p["error"].get<std::string>().empty();
        }
        CHECK(saw_ok);
        CHECK(saw_broken);
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

    // --- misc requests -----------------------------------------------------------
    r = request(c, "set_wifi", {{"ssid", "X"}, {"channel", 6}});
    CHECK(!r["ok"].get<bool>() && r["error"].get<std::string>().find("not available") != std::string::npos);
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
    for (uint32_t seq = 1; seq <= 3; ++seq) {
        const json header = {{"seq", seq}, {"tof_mm", {100, 200, 300}}, {"state_name", "CRUISE"}};
        telemetry.publish(std::make_shared<const std::vector<uint8_t>>(net::encode_telemetry_frame(header.dump(), {})));
        std::this_thread::sleep_for(5ms);
    }
    for (uint32_t seq = 1; seq <= 3; ++seq) {
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
    test_version_store_names();
    test_servers(argv[1]);
    return check::summary("test_network");
}
