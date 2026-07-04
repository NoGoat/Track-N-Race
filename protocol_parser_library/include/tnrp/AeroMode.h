#pragma once

#include <cstdint>
#include <string>

namespace tnrp {

// Overtaking-aid mode for a given packet format. F1 24 and F1 25 use DRS; F1 26
// replaces it with active-aero "Straight Line Mode" (SLM). Shipped on the
// protocol_status control row so the renderer picks the matching track-map
// overlay (DRS zones vs SLM dry/wet zones) for the active game year.
inline std::string aeroMode(uint16_t format) {
    return format >= 2026 ? "slm" : "drs";
}

} // namespace tnrp
