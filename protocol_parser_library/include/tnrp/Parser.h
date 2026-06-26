#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "tnrp/Config.h"

namespace tnrp {

// Pure decode layer: format detection + manual override + debounce + rate
// limiting + dispatch to the versioned F1_24/F1_25 parsers. Holds no I/O and no
// Qt; it is the consolidated equivalent of the Electron protocolDispatcher.ts
// and the native MainWindow::processPacket front half.
//
// Persistence of the last-detected format is intentionally NOT done here — the
// engine surfaces protocol_status control rows and the host (Electron) persists
// them. This keeps the parser a pure function of its inputs + override state.
class Parser {
public:
    explicit Parser(Override ovr = Override::Auto);

    void setOverride(Override ovr);
    Override override_() const { return override_v_; }

    // Format the parser is currently routing with (null == nothing detected yet,
    // represented as 0). Used by the engine to feed the writer's session rotation.
    uint16_t activeFormat() const { return activeFormat_; }

    struct Result {
        bool                         dropped = false;  // rate-limited / unknown format
        uint16_t                     format  = 0;       // effective format used (2024/2025)
        uint8_t                      packetId = 0;
        float                        sessionTime = -1.0f;
        std::vector<nlohmann::json>  rows;              // parsed telemetry rows
        std::vector<nlohmann::json>  control;           // protocol_status / protocol_warning
    };

    // Decode one raw UDP datagram. `ts` is the ISO timestamp to stamp on rows.
    Result feed(const uint8_t* data, int length, const std::string& ts);

    // The current protocol_status control row (detected/active format, override,
    // capabilities). The engine emits this on construction and after setOverride.
    nlohmann::json statusRow() const;

    // Reset rate-limit / debounce state (e.g. on UDP restart).
    void reset();

private:
    Override  override_v_       = Override::Auto;
    uint16_t  detectedFormat_   = 0;   // last debounced detection (0 == none)
    uint16_t  activeFormat_     = 0;   // what we route with right now
    uint16_t  debounceCandidate_= 0;
    int       debounceCount_    = 0;

    // Last protocol_warning state emitted, so we only surface a control row on a
    // change (the Electron dispatcher re-emitted one per packet; over a pipe that
    // would be wasteful).
    bool      warnActive_       = false;
    uint16_t  warnForced_       = 0;

    // Rate-limit state, keyed by packetId.
    std::unordered_map<int, uint32_t> lastFrameId_;  // frame-sampled packets
    std::unordered_map<int, uint64_t> lastSlowMs_;   // wall-clock throttled packets

    uint16_t effectiveFormat(uint16_t incoming) const;
};

} // namespace tnrp
