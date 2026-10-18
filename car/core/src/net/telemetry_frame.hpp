// Telemetry frame encoding (car -> PC), see docs 5.3:
//   "RMCT" | u8 version=1 | u16 header_len | u32 payload_len | header JSON | JPEG
// Little-endian.  Mirrors pc/RumiCar.Protocol/Telemetry/TelemetryFrame.cs.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rumicar::net {

constexpr uint8_t kTelemetryVersion = 1;
constexpr size_t kTelemetryFixedHeaderBytes = 4 + 1 + 2 + 4;
constexpr size_t kTelemetryMaxHeaderBytes = 16 * 1024;
constexpr size_t kTelemetryMaxPayloadBytes = 4 * 1024 * 1024;

// Throws std::runtime_error when the header or payload exceed the limits.
std::vector<uint8_t> encode_telemetry_frame(const std::string& header_json, const std::vector<uint8_t>& jpeg);

} // namespace rumicar::net
