#pragma once

#include <cstdint>
#include <optional>

namespace tnrp {

// Whether the active power-unit regulations include an MGU-H. The F1 26 UDP
// layout retains the legacy harvest field for packet compatibility, but 2026
// cars have no MGU-H, so consumers must not present that series in the UI.
inline bool hasMguh(uint16_t format) {
    return format == 2024 || format == 2025;
}

inline constexpr int F1_26_FORMULA = 13;

// A 2026 packet can describe pre-2026 cars. Keep decoding with the 2026 wire
// layout, but present those sessions with the pre-2026 UI. 2025 and 2026 cars
// never share a session, so one car's rules decide for the whole session.
// Sources, most direct first:
//   1. Car Telemetry 2 m_2026Regulations (live, or stored in V6 recordings);
//   2. Session m_formula, 13 = F1 26 (recordings made before (1) was stored);
//   3. the protocol alone (recordings made before Formula was captured).
inline uint16_t presentationFormat(uint16_t protocol, std::optional<bool> regulations2026,
                                   std::optional<int> formula) {
    if (protocol != 2026) return protocol;
    if (regulations2026) return *regulations2026 ? 2026 : 2025;
    if (formula) return *formula == F1_26_FORMULA ? 2026 : 2025;
    return 2026;
}

// m_2026Regulations from a 2026 Car Telemetry 2 packet (id 16): 24 entries of
// 10 bytes after the 29-byte header, the flag at byte 8 of each. Reads the
// player's car, which is always a real car; slots beyond the grid report 0.
// A spectator (player index 255) falls back to car 0.
inline std::optional<bool> regulations2026FromCarTelemetry2(const uint8_t* data, int length) {
    constexpr int header = 29, entry = 10, cars = 24, flag = 8;
    if (!data || length < header + cars * entry) return std::nullopt;
    const int car = data[27] < cars ? data[27] : 0;
    return data[header + car * entry + flag] == 1;
}

} // namespace tnrp
