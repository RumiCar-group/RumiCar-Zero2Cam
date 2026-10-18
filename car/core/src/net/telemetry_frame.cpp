#include "telemetry_frame.hpp"

#include <cstring>
#include <stdexcept>

namespace rumicar::net {

std::vector<uint8_t> encode_telemetry_frame(const std::string& header_json, const std::vector<uint8_t>& jpeg) {
    if (header_json.empty() || header_json.size() > kTelemetryMaxHeaderBytes) throw std::runtime_error("telemetry header size out of range");
    if (jpeg.size() > kTelemetryMaxPayloadBytes) throw std::runtime_error("telemetry payload too large");

    std::vector<uint8_t> out(kTelemetryFixedHeaderBytes + header_json.size() + jpeg.size());
    uint8_t* p = out.data();
    std::memcpy(p, "RMCT", 4);
    p[4] = kTelemetryVersion;
    const auto hlen = static_cast<uint16_t>(header_json.size());
    const auto plen = static_cast<uint32_t>(jpeg.size());
    p[5] = static_cast<uint8_t>(hlen & 0xFF);
    p[6] = static_cast<uint8_t>((hlen >> 8) & 0xFF);
    p[7] = static_cast<uint8_t>(plen & 0xFF);
    p[8] = static_cast<uint8_t>((plen >> 8) & 0xFF);
    p[9] = static_cast<uint8_t>((plen >> 16) & 0xFF);
    p[10] = static_cast<uint8_t>((plen >> 24) & 0xFF);
    std::memcpy(p + kTelemetryFixedHeaderBytes, header_json.data(), header_json.size());
    if (!jpeg.empty()) std::memcpy(p + kTelemetryFixedHeaderBytes + header_json.size(), jpeg.data(), jpeg.size());
    return out;
}

} // namespace rumicar::net
