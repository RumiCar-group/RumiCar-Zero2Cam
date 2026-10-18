// Control-channel requests (docs 5.2) applied to the car state and the
// version store.  Runs on the control server's client thread.
#pragma once

#include <chrono>
#include <string>

#include <nlohmann/json.hpp>

#include "car_state.hpp"
#include "version_store.hpp"

namespace rumicar {

class RequestHandler {
public:
    RequestHandler(CarState& state, const VersionStore& store, std::string car_name, std::string core_version,
                   std::chrono::milliseconds activation_wait = std::chrono::milliseconds(5000));

    // Returns the result (json null = none).  Throws net::RequestError for ok=false.
    nlohmann::json handle(const std::string& type, const nlohmann::json& params);

    static constexpr int kProtocolVersion = 1;
    static constexpr int kMaxLogLines = 500;

private:
    nlohmann::json activation_outcome(const char* what);

    CarState& state_;
    const VersionStore& store_;
    std::string car_name_;
    std::string core_version_;
    std::chrono::milliseconds activation_wait_;
};

} // namespace rumicar
