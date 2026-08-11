#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

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
        bool                     dropped    = false;  // invalid format or duplicate frame
        uint16_t                 format     = 0;      // effective format used (2024/2025)
        uint8_t                  packetId   = 0;
        float                    sessionTime = -1.0f;
        std::vector<std::string> rows;                // cold rows: serialised JSON (record + live)
        std::vector<std::string> control;             // serialised protocol_status / protocol_warning
        std::vector<std::string> hotJson;             // hot rows as JSON (only when wantHotJson)
        std::vector<uint8_t>     binary;              // hot rows packed for the live binary channel
    };

    // Decode one raw UDP datagram. `ts` is the ISO timestamp to stamp on rows.
    // `wantHotJson` (true == logging on) makes the hot 60 Hz rows additionally
    // serialised to JSON in `hotJson` so they can be recorded; the binary form is
    // always produced for the live channel. Packet cadence is controlled by the
    // game's UDP send-rate setting; the parser does not wall-clock throttle it.
    Result feed(const uint8_t* data, int length, const std::string& ts,
                bool wantHotJson, uint32_t outputRowMask = 0xFFFFFFFFu);

    // The current protocol_status control row as a serialised JSON string.
    // The engine emits this on construction and after setOverride.
    std::string statusRow() const;

    // protocol_status row for a fixed packet format, independent of any live
    // parser state. Playback uses this to label a recorded clip with its own
    // format's i18n catalog / capabilities / aero mode.
    static std::string statusRowForFormat(uint16_t format);

    // Reset duplicate-frame / debounce state (e.g. on UDP restart).
    void reset();

private:
    Override  override_v_        = Override::Auto;
    uint16_t  detectedFormat_    = 0;
    uint16_t  activeFormat_      = 0;
    uint16_t  debounceCandidate_ = 0;
    int       debounceCount_     = 0;

    bool      warnActive_  = false;
    uint16_t  warnForced_  = 0;

    // Fixed tables indexed by packetId (0..15) for exact duplicate-frame checks.
    std::array<uint32_t, 16> lastFrameId_{};
    std::array<bool,     16> haveFrameId_{};

    uint16_t effectiveFormat(uint16_t incoming) const;
};

} // namespace tnrp
