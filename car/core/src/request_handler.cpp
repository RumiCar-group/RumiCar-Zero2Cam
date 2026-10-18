#include "request_handler.hpp"

#include "log.hpp"
#include "net/control_server.hpp"

namespace rumicar {

using json = nlohmann::json;
using net::RequestError;

namespace {

int require_int(const json& params, const char* key, int lo, int hi) {
    const auto it = params.find(key);
    if (it == params.end() || !it->is_number_integer()) throw RequestError(std::string("missing integer '") + key + "'");
    const auto v = it->get<long long>();
    if (v < lo || v > hi) throw RequestError(std::string("'") + key + "' must be in [" + std::to_string(lo) + ", " + std::to_string(hi) + "]");
    return static_cast<int>(v);
}

int optional_int(const json& params, const char* key, int fallback, int lo, int hi) {
    if (params.find(key) == params.end()) return fallback;
    return require_int(params, key, lo, hi);
}

std::string require_string(const json& params, const char* key) {
    const auto it = params.find(key);
    if (it == params.end() || !it->is_string()) throw RequestError(std::string("missing string '") + key + "'");
    return it->get<std::string>();
}

std::string require_kind(const json& params) {
    const std::string kind = require_string(params, "kind");
    if (kind != "logic" && kind != "pack") throw RequestError("'kind' must be 'logic' or 'pack'");
    return kind;
}

json versions_json(const std::vector<InstalledVersion>& list) {
    json arr = json::array();
    for (const auto& v : list) {
        arr.push_back({{"name", v.dir_name}, {"id", v.id}, {"version", v.version}, {"verified", v.verified}, {"error", v.error}});
    }
    return arr;
}

} // namespace

RequestHandler::RequestHandler(CarState& state, const VersionStore& store, std::string car_name, std::string core_version,
                               std::chrono::milliseconds activation_wait)
    : state_(state), store_(store), car_name_(std::move(car_name)), core_version_(std::move(core_version)),
      activation_wait_(activation_wait) {}

json RequestHandler::activation_outcome(const char* what) {
    const uint64_t gen = state_.request_activation();
    ActivationResult r;
    if (!state_.wait_activation(gen, activation_wait_, r)) {
        throw RequestError(std::string(what) + ": the core did not finish verifying within " +
                           std::to_string(activation_wait_.count()) + " ms");
    }
    if (!r.ok) throw RequestError(std::string(what) + " failed, keeping the running versions: " + r.error);
    return json{{"ok", true}, {"logic_dir", r.logic_dir}, {"pack_dir", r.pack_dir}};
}

json RequestHandler::handle(const std::string& type, const json& params) {
    const auto now = CarState::Clock::now();

    if (type == "hello") {
        const int protocol = require_int(params, "protocol", 0, 1000000);
        if (protocol != kProtocolVersion)
            throw RequestError("unsupported protocol version " + std::to_string(protocol) + " (car speaks " + std::to_string(kProtocolVersion) + ")");
        std::string client = "?";
        if (const auto it = params.find("client"); it != params.end() && it->is_string()) client = it->get<std::string>();
        log::info("control client hello: " + client);
        return json{{"car", car_name_}, {"core_version", core_version_}, {"protocol", kProtocolVersion}};
    }
    if (type == "get_status") {
        return state_.status_json();
    }
    if (type == "set_mode") {
        DriveMode mode;
        const std::string name = require_string(params, "mode");
        if (!parse_drive_mode(name, mode)) throw RequestError("unknown mode '" + name + "'");
        state_.set_mode(mode, now);
        log::info("mode set to " + name);
        return nullptr;
    }
    if (type == "manual_drive") {
        const int steer = require_int(params, "steer", -1, 1);
        const int drive = require_int(params, "drive_permille", -1000, 1000);
        state_.heartbeat(now);
        if (!state_.set_manual(static_cast<int8_t>(steer), drive)) throw RequestError("not in manual mode");
        return nullptr;
    }
    if (type == "heartbeat") {
        state_.heartbeat(now);
        return nullptr;
    }
    if (type == "estop") {
        if (state_.set_estop(true, "request")) log::warn("emergency stop requested by the PC");
        return nullptr;
    }
    if (type == "estop_clear") {
        if (state_.set_estop(false, "")) log::info("emergency stop cleared by the PC");
        return nullptr;
    }
    if (type == "list_versions") {
        return json{
            {"logic", versions_json(store_.list("logic"))},
            {"packs", versions_json(store_.list("pack"))},
            {"current", {{"logic", store_.link_target("current", "logic")}, {"pack", store_.link_target("current", "pack")}}},
            {"previous", {{"logic", store_.link_target("previous", "logic")}, {"pack", store_.link_target("previous", "pack")}}},
        };
    }
    if (type == "activate") {
        const std::string kind = require_kind(params);
        const std::string name = require_string(params, "name");
        try {
            store_.activate(kind, name);
        } catch (const std::exception& e) {
            throw RequestError(e.what());
        }
        log::info("activation requested by the PC: " + kind + " " + name);
        return activation_outcome("activate");
    }
    if (type == "rollback") {
        const std::string kind = require_kind(params);
        try {
            store_.rollback(kind);
        } catch (const std::exception& e) {
            throw RequestError(e.what());
        }
        log::info("rollback requested by the PC: " + kind);
        return activation_outcome("rollback");
    }
    if (type == "set_wifi") {
        throw RequestError("set_wifi is not available in this version of the car software");
    }
    if (type == "get_log") {
        const int lines = optional_int(params, "lines", 100, 1, kMaxLogLines);
        return json{{"lines", log::recent(static_cast<size_t>(lines))}};
    }
    throw RequestError("unknown request type '" + type + "'");
}

} // namespace rumicar
