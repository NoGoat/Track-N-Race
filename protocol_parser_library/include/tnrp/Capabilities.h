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

// A 2026 packet can describe legacy-formula cars. Keep decoding with the 2026
// wire layout, but present those sessions with the pre-2026 UI. Recordings made
// before Formula was captured deliberately retain the existing 2026 default.
inline uint16_t presentationFormat(uint16_t protocol, std::optional<int> formula) {
    return protocol == 2026 && formula && *formula != F1_26_FORMULA ? 2025 : protocol;
}

} // namespace tnrp
