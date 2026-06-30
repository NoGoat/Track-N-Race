#include "tnrp/Parser.h"

#include <array>
#include <chrono>

#include "tnrp/control_rows.h"
#include "tnrp/Labels.h"

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
static constexpr int PID_LAP_DATA     = 2;
static constexpr int PID_EVENT        = 3;
static constexpr int PID_PARTICIPANTS = 4;
static constexpr int PID_CAR_TEL      = 6;
static constexpr int PID_CAR_STATUS   = 7;
static constexpr int PID_CAR_DAMAGE   = 10;
static constexpr int PID_MOTION_EX    = 13;

// Packet IDs are a tiny dense range (0..13); fixed arrays indexed by id avoid a
// hash + pointer-chase per datagram. Frame-sampled packets dedupe on frameId;
// the rest are time-rate-limited (0 == no limit, default 500 ms for unlisted).
static constexpr int PID_TABLE_SIZE = 16;

static constexpr std::array<bool, PID_TABLE_SIZE> makeFrameSampled() {
    std::array<bool, PID_TABLE_SIZE> a{};
    a[PID_MOTION]    = true;
    a[PID_CAR_TEL]   = true;
    a[PID_MOTION_EX] = true;
    return a;
}
static constexpr std::array<int, PID_TABLE_SIZE> makeSlowRateMs() {
    std::array<int, PID_TABLE_SIZE> a{};
    for (int i = 0; i < PID_TABLE_SIZE; ++i) a[i] = 500;  // default for unlisted
    a[PID_SESSION]      = 0;
    a[PID_EVENT]        = 0;
    a[PID_PARTICIPANTS] = 5000;
    // PID_MOTION / PID_CAR_TEL / PID_MOTION_EX are frame-sampled, never read here.
    return a;
}
static constexpr std::array<bool, PID_TABLE_SIZE> kFrameSampled = makeFrameSampled();
static constexpr std::array<int,  PID_TABLE_SIZE> kSlowRateMs   = makeSlowRateMs();

static uint64_t nowMs() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

Parser::Parser(Override ovr) : override_v_(ovr) {
    if (ovr == Override::F1_26) activeFormat_ = 2026;
    else if (ovr == Override::F1_25) activeFormat_ = 2025;
    else if (ovr == Override::F1_24) activeFormat_ = 2024;
}

void Parser::reset() {
    lastFrameId_.fill(0);
    haveFrameId_.fill(false);
    lastSlowMs_.fill(0);
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
    int gameYear = activeFormat_ == 2026 ? 26
                 : (activeFormat_ == 2025 ? 25
                 : (activeFormat_ == 2024 ? 24 : -1));
    // 2026 is a superset of 2025 — these capabilities hold from 2025 onward.
    bool isF125OrLater = activeFormat_ >= 2025;
    ProtocolStatusRow row;
    if (gameYear >= 0)   row.capabilities.gameYear = gameYear;
    row.capabilities.hasBlisters     = isF125OrLater;
    row.capabilities.hasLiveryColors = isF125OrLater;
    row.capabilities.hasLapPositions = isF125OrLater;
    if (detectedFormat_) row.detected_format = detectedFormat_;
    if (activeFormat_)   row.active_format   = activeFormat_;
    row.override_ = toString(override_v_);
    // Ship the i18n catalog for the active format (default to 2025 before any
    // packet is seen) so the renderer always has labels to resolve against.
    const auto& cat = labelsFor(activeFormat_ ? activeFormat_ : 2025);
    row.labels.insert(cat.all().begin(), cat.all().end());
    return writeJsonNullable(row);
}

Parser::Result Parser::feed(const uint8_t* data, int length, const std::string& ts, bool wantHotJson) {
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
            if (prevActive != activeFormat_) reset();
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

    // ── Rate limiting ────────────────────────────────────────────────────────
    uint8_t  packetId = data[6];
    float    sessionTime = ReadFloat(data, 15);
    uint32_t frameId  = ReadUInt32(data, 23);
    r.format      = eff;
    r.packetId    = packetId;
    r.sessionTime = sessionTime;

    if (packetId < PID_TABLE_SIZE) {
        if (kFrameSampled[packetId]) {
            if (haveFrameId_[packetId] && lastFrameId_[packetId] == frameId) {
                r.dropped = true; return r;
            }
            haveFrameId_[packetId] = true;
            lastFrameId_[packetId] = frameId;
        } else {
            int rateMs = kSlowRateMs[packetId];
            if (rateMs > 0) {
                uint64_t now = nowMs();
                // lastSlowMs_ starts at 0; (now - 0) always exceeds rateMs, so the
                // first packet of each type is never spuriously dropped.
                if (lastSlowMs_[packetId] != 0 && (now - lastSlowMs_[packetId]) < (uint64_t)rateMs) {
                    r.dropped = true; return r;
                }
                lastSlowMs_[packetId] = now;
            }
        }
    }
    // packetId >= PID_TABLE_SIZE: unknown id; ParsePacket has no case and returns {}.

    // ── Dispatch to the versioned parser ─────────────────────────────────────
    PacketHeader hdr;
    hdr.packetFormat   = eff;
    hdr.packetId       = packetId;
    hdr.sessionTime    = sessionTime;
    hdr.overallFrameId = frameId;
    hdr.playerCarIndex = data[27];

    HotOut hot;
    hot.wantHotJson = wantHotJson;
    r.rows = (eff == 2024) ? F1_24::ParsePacket(data, length, hdr, ts, hot)
           : (eff == 2026) ? F1_26::ParsePacket(data, length, hdr, ts, hot)
                           : F1_25::ParsePacket(data, length, hdr, ts, hot);
    r.hotJson = std::move(hot.hotJson);
    r.binary  = std::move(hot.binary);
    return r;
}

} // namespace tnrp
