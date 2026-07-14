#pragma once

#include <cstdint>

namespace tnrp {

// Whether the active power-unit regulations include an MGU-H. The F1 26 UDP
// layout retains the legacy harvest field for packet compatibility, but 2026
// cars have no MGU-H, so consumers must not present that series in the UI.
inline bool hasMguh(uint16_t format) {
    return format == 2024 || format == 2025;
}

} // namespace tnrp
