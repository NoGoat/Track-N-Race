#include "tnrp/Parser.h"

#include <chrono>
#include <unordered_map>
#include <unordered_set>

#include "tnrp/control_rows.h"

#include "protocols/protocol.h"
#include "protocols/f1_24.h"
#include "protocols/f1_25.h"

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

static const std::unordered_set<int> FRAME_SAMPLED = {
    PID_MOTION, PID_CAR_TEL, PID_MOTION_EX
};
static const std::unordered_map<int, int> SLOW_RATE_MS = {
    { PID_SESSION, 0 }, { PID_LAP_DATA, 500 }, { PID_CAR_STATUS, 500 },
    { PID_CAR_DAMAGE, 500 }, { PID_PARTICIPANTS, 5000 }, { PID_EVENT, 0 }
};

static uint64_t nowMs() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

Parser::Parser(Override ovr) : override_v_(ovr) {
    if (ovr == Override::F1_25) activeFormat_ = 2025;
    else if (ovr == Override::F1_24) activeFormat_ = 2024;
}

void Parser::reset() {
    lastFrameId_.clear();
    lastSlowMs_.clear();
}

void Parser::setOverride(Override ovr) {
    override_v_ = ovr;
    if (ovr == Override::F1_25)      activeFormat_ = 2025;
    else if (ovr == Override::F1_24) activeFormat_ = 2024;
    else                             activeFormat_ = detectedFormat_;
    reset();
    warnActive_ = false;
    warnForced_ = 0;
}

uint16_t Parser::effectiveFormat(uint16_t incoming) const {
    if (override_v_ == Override::F1_25) return 2025;
    if (override_v_ == Override::F1_24) return 2024;
    if (detectedFormat_) return detectedFormat_;
    if (activeFormat_)   return activeFormat_;
    return incoming;
}

std::string Parser::statusRow() const {
    int gameYear = activeFormat_ == 2025 ? 25 : (activeFormat_ == 2024 ? 24 : -1);
    bool isF125 = activeFormat_ == 2025;
    ProtocolStatusRow row;
    if (gameYear >= 0)   row.capabilities.gameYear = gameYear;
    row.capabilities.hasBlisters     = isF125;
    row.capabilities.hasLiveryColors = isF125;
    row.capabilities.hasLapPositions = isF125;
    if (detectedFormat_) row.detected_format = detectedFormat_;
    if (activeFormat_)   row.active_format   = activeFormat_;
    row.override_ = toString(override_v_);
    return writeJsonNullable(row);
}

Parser::Result Parser::feed(const uint8_t* data, int length, const std::string& ts) {
    Result r;
    if (length < HEADER_SIZE) { r.dropped = true; return r; }

    uint16_t incoming = ReadUInt16(data, 0);
    if (incoming != 2024 && incoming != 2025) { r.dropped = true; return r; }

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

    if (FRAME_SAMPLED.count(packetId)) {
        auto it = lastFrameId_.find(packetId);
        if (it != lastFrameId_.end() && it->second == frameId) { r.dropped = true; return r; }
        lastFrameId_[packetId] = frameId;
    } else {
        int rateMs = 500;
        auto itL = SLOW_RATE_MS.find(packetId);
        if (itL != SLOW_RATE_MS.end()) rateMs = itL->second;
        if (rateMs > 0) {
            uint64_t now = nowMs();
            auto itT = lastSlowMs_.find(packetId);
            if (itT != lastSlowMs_.end() && (now - itT->second) < (uint64_t)rateMs) {
                r.dropped = true; return r;
            }
            lastSlowMs_[packetId] = now;
        }
    }

    // ── Dispatch to the versioned parser ─────────────────────────────────────
    PacketHeader hdr;
    hdr.packetFormat   = eff;
    hdr.packetId       = packetId;
    hdr.sessionTime    = sessionTime;
    hdr.overallFrameId = frameId;
    hdr.playerCarIndex = data[27];

    r.rows = (eff == 2024)
        ? F1_24::ParsePacket(data, length, hdr, ts)
        : F1_25::ParsePacket(data, length, hdr, ts);
    return r;
}

} // namespace tnrp
