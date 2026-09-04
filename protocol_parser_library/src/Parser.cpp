#include "tnrp/Parser.h"

#include <array>
#include <cmath>
#include <cstring>
#include <iterator>
#include "tnrp/control_rows.h"
#include "tnrp/Labels.h"
#include "tnrp/CardColors.h"
#include "tnrp/AeroMode.h"
#include "tnrp/Capabilities.h"

#include "protocols/protocol.h"
#include "protocols/f1_24.h"
#include "protocols/f1_25.h"
#include "protocols/f1_26.h"

namespace tnrp {

static constexpr int HEADER_SIZE   = 29;
static constexpr int DEBOUNCE_COUNT = 3;

// Packet IDs
static constexpr int PID_MOTION       = 0;
static constexpr int PID_SESSION      = 1;
static constexpr int PID_CAR_TEL      = 6;
static constexpr int PID_MOTION_EX    = 13;

// Packet IDs are a tiny dense range (0..13); fixed arrays indexed by id avoid a
// hash + pointer-chase per datagram. The game controls packet cadence through
// its UDP send-rate setting. We only discard exact duplicate hot-row frame IDs.
static constexpr int PID_TABLE_SIZE = 16;

static constexpr std::array<bool, PID_TABLE_SIZE> makeFrameSampled() {
    std::array<bool, PID_TABLE_SIZE> a{};
    a[PID_MOTION]    = true;
    a[PID_CAR_TEL]   = true;
    a[PID_MOTION_EX] = true;
    return a;
}
static constexpr std::array<bool, PID_TABLE_SIZE> kFrameSampled = makeFrameSampled();

Parser::Parser(Override ovr) : override_v_(ovr) {
    if (ovr == Override::F1_26) activeFormat_ = 2026;
    else if (ovr == Override::F1_25) activeFormat_ = 2025;
    else if (ovr == Override::F1_24) activeFormat_ = 2024;
}

void Parser::reset() {
    lastFrameId_.fill(0);
    haveFrameId_.fill(false);
    formula_.reset();
}

void Parser::setOverride(Override ovr) {
    override_v_ = ovr;
    if (ovr == Override::F1_26)      activeFormat_ = 2026;
    else if (ovr == Override::F1_25) activeFormat_ = 2025;
    else if (ovr == Override::F1_24) activeFormat_ = 2024;
    else                             activeFormat_ = detectedFormat_;
    reset();
    warnActive_ = false;
    warnForced_ = 0;
}

uint16_t Parser::effectiveFormat(uint16_t incoming) const {
    if (override_v_ == Override::F1_26) return 2026;
    if (override_v_ == Override::F1_25) return 2025;
    if (override_v_ == Override::F1_24) return 2024;
    if (detectedFormat_) return detectedFormat_;
    if (activeFormat_)   return activeFormat_;
    return incoming;
}

std::string Parser::statusRow() const {
    const uint16_t displayFormat = presentationFormat(activeFormat_, formula_);
    int gameYear = displayFormat == 2026 ? 26
                 : (displayFormat == 2025 ? 25
                 : (displayFormat == 2024 ? 24 : -1));
    // 2026 is a superset of 2025 — these capabilities hold from 2025 onward.
    bool isF125OrLater = displayFormat >= 2025;
    ProtocolStatusRow row;
    if (gameYear >= 0)   row.capabilities.gameYear = gameYear;
    row.capabilities.hasBlisters     = isF125OrLater;
    row.capabilities.hasLiveryColors = isF125OrLater;
    row.capabilities.hasLapPositions = isF125OrLater;
    row.capabilities.hasMguh         = hasMguh(displayFormat);
    if (detectedFormat_) row.detected_format = detectedFormat_;
    if (activeFormat_)   row.active_format   = activeFormat_;
    if (displayFormat)   row.presentation_format = displayFormat;
    row.formula = formula_;
    row.override_ = toString(override_v_);
    // Ship the i18n catalog for the active format (default to 2025 before any
    // packet is seen) so the renderer always has labels to resolve against.
    const auto& cat = labelsFor(displayFormat ? displayFormat : 2025);
    row.labels.insert(cat.all().begin(), cat.all().end());
    row.cardColors = cardColors();   // format-independent colour model
    row.aero_mode  = aeroMode(displayFormat ? displayFormat : 2025);
    return writeJsonNullable(row);
}

std::string Parser::statusRowForFormat(uint16_t format, std::optional<int> formula) {
    const uint16_t displayFormat = presentationFormat(format, formula);
    int gameYear = displayFormat == 2026 ? 26
                 : (displayFormat == 2025 ? 25
                 : (displayFormat == 2024 ? 24 : -1));
    bool isF125OrLater = displayFormat >= 2025;
    ProtocolStatusRow row;
    if (gameYear >= 0)   row.capabilities.gameYear = gameYear;
    row.capabilities.hasBlisters     = isF125OrLater;
    row.capabilities.hasLiveryColors = isF125OrLater;
    row.capabilities.hasLapPositions = isF125OrLater;
    row.capabilities.hasMguh         = hasMguh(displayFormat);
    row.detected_format = format;
    row.active_format   = format;
    row.presentation_format = displayFormat;
    row.formula = formula;
    row.override_ = toString(Override::Auto);
    const auto& cat = labelsFor(displayFormat);
    row.labels.insert(cat.all().begin(), cat.all().end());
    row.cardColors = cardColors();
    row.aero_mode  = aeroMode(displayFormat);
    return writeJsonNullable(row);
}

Parser::Result Parser::feed(const uint8_t* data, int length, const std::string& ts,
                            bool wantHotJson, uint32_t outputRowMask) {
    Result r;
    if (length < HEADER_SIZE) { r.dropped = true; return r; }

    uint16_t incoming = ReadUInt16(data, 0);
    if (incoming != 2024 && incoming != 2025 && incoming != 2026) { r.dropped = true; return r; }

    // ── Debounce auto-detection (3 consecutive same-format packets) ──────────
    if (incoming != debounceCandidate_) {
        debounceCandidate_ = incoming;
        debounceCount_     = 1;
    } else {
        debounceCount_++;
    }

    if (debounceCount_ >= DEBOUNCE_COUNT && incoming != detectedFormat_) {
        uint16_t prev   = detectedFormat_;
        detectedFormat_ = incoming;
        if (override_v_ == Override::Auto) {
            uint16_t prevActive = activeFormat_;
            activeFormat_ = incoming;
            if (prevActive != activeFormat_) {
                reset();
            }
        }
        if (prev != detectedFormat_) r.control.push_back(statusRow());
    }

    uint16_t eff = effectiveFormat(incoming);
    if (override_v_ != Override::Auto) activeFormat_ = eff;
    else if (activeFormat_ == 0)       activeFormat_ = eff;

    // ── Mismatch warning (only on state change, not per packet) ──────────────
    bool mismatch = (override_v_ != Override::Auto) && (incoming != eff);
    if (mismatch) {
        if (!warnActive_ || warnForced_ != eff) {
            warnActive_ = true;
            warnForced_ = eff;
            ProtocolWarningRow w;
            w.detected_format = incoming;
            w.forced_format   = eff;
            r.control.push_back(writeJsonNullable(w));
        }
    } else if (warnActive_) {
        warnActive_ = false;
        warnForced_ = 0;
        ProtocolWarningRow w;   // both fields nullopt → emitted as null
        r.control.push_back(writeJsonNullable(w));
    }

    // ── Exact duplicate-frame rejection ─────────────────────────────────────
    uint8_t  packetId = data[6];
    float    sessionTime = ReadFloat(data, 15);
    uint32_t frameId  = ReadUInt32(data, 23);
    r.format      = eff;
    r.packetId    = packetId;
    r.sessionTime = sessionTime;

    // Formula is presentation state, not a wire-format selector. A known
    // non-F1-26 formula on the 2026 protocol switches labels/capabilities back
    // to the legacy presentation; no captured value preserves the 2026 default.
    if (eff == 2026 && packetId == PID_SESSION && length > 37) {
        const uint16_t previousPresentation = presentationFormat(activeFormat_, formula_);
        const std::optional<int> previousFormula = formula_;
        formula_ = static_cast<int>(data[37]);
        if (formula_ != previousFormula ||
            presentationFormat(activeFormat_, formula_) != previousPresentation)
            r.control.push_back(statusRow());
    }

    if (packetId < PID_TABLE_SIZE && kFrameSampled[packetId]) {
        if (haveFrameId_[packetId] && lastFrameId_[packetId] == frameId) {
            r.dropped = true; return r;
        }
        haveFrameId_[packetId] = true;
        lastFrameId_[packetId] = frameId;
    }
    // packetId >= PID_TABLE_SIZE: unknown id; ParsePacket has no case and returns {}.

    // ── Dispatch to the versioned parser ─────────────────────────────────────
    PacketHeader hdr;
    hdr.packetFormat   = eff;
    hdr.packetId       = packetId;
    hdr.sessionTime    = sessionTime;
    hdr.overallFrameId = frameId;
    hdr.playerCarIndex = data[27];

    // FLBK is identical in every supported protocol: the event payload carries
    // the authoritative frame/time to which the game restored. Decode it here
    // so timeline correction does not depend on a UI's event subscription and
    // does not mistake the monotonic overallFrameIdentifier for game time.
    if (packetId == 3 && length >= HEADER_SIZE + 12 &&
        std::memcmp(data + HEADER_SIZE, "FLBK", 4) == 0) {
        const uint32_t targetFrame = ReadUInt32(data, HEADER_SIZE + 4);
        const float targetTime = ReadFloat(data, HEADER_SIZE + 8);
        if (std::isfinite(targetTime) && targetTime >= 0.0f) {
            r.rewindSessionTime = targetTime;
            r.rewindFrameIdentifier = targetFrame;
            RaceEventRow ev;
            ev.ts = ts;
            ev.session_time = targetTime;
            ev.code = "FLBK";
            ev.flashback_frame_identifier = targetFrame;
            ev.flashback_session_time = targetTime;
            r.rows.push_back(writeJsonNullable(ev));
            return r;
        }
    }

    // The header/detection/duplicate state above always advances. When no
    // recording or JSON-only consumer needs the body, skip parsing packets that
    // cannot produce a currently subscribed logical row family.
    static constexpr uint32_t PACKET_ROWS[17] = {
        (1u << 11) | (1u << 13), // Motion + Positions
        (1u << 5),               // Session
        (1u << 4) | (1u << 7),   // Lap + Timing
        (1u << 6),               // Event
        (1u << 8),               // Participants
        0,
        (1u << 1),               // Car Telemetry
        (1u << 2) | (1u << 9),   // Car Status + All Status
        0,
        0,
        (1u << 3),               // Car Damage
        (1u << 14),              // Session History Fastest
        (1u << 10),              // Tyre Sets
        (1u << 12),              // Motion Ex
        0, 0,
        (1u << 1),               // F1 26 Telemetry 2 updates telemetry state
    };
    const uint32_t packetRows = packetId < std::size(PACKET_ROWS)
        ? PACKET_ROWS[packetId] : 0;
    if (!wantHotJson && (packetRows & outputRowMask) == 0) return r;

    HotOut hot;
    hot.wantHotJson = wantHotJson;
    hot.outputRowMask = outputRowMask;
    r.rows = (eff == 2024) ? F1_24::ParsePacket(data, length, hdr, ts, hot)
           : (eff == 2026) ? F1_26::ParsePacket(data, length, hdr, ts, hot)
                           : F1_25::ParsePacket(data, length, hdr, ts, hot);
    r.hotJson = std::move(hot.hotJson);
    r.binary  = std::move(hot.binary);
    return r;
}

} // namespace tnrp
