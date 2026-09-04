#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <type_traits>
#include <vector>

#include "tnrp/rows.h"

// Binary wire format for the hot 60 Hz rows (telemetry / motion / positions /
// motion_ex). These are all-numeric, so instead of serialising them to JSON,
// widening to UTF-16 across the N-API boundary and JSON.parse-ing them in the
// renderer, the engine packs them into fixed-layout little-endian records and
// ships the raw bytes. The renderer decodes them with a DataView (see
// electron-frontend/src/renderer/src/lib/decodeBinaryBatch.ts — keep the two in sync).
//
// Layout: a batch is a concatenation of records, each `u8 tag` + fixed fields.
// `ts` is intentionally omitted (the live charts key off session_time; the only
// consumers of `ts` are the cold session/lap/timing rows, which stay JSON).
//
// Endianness: bytes are written host-native. The whole codebase already assumes
// a little-endian host (see ReadFloat in protocols/protocol.h); the decoder reads
// little-endian to match.

namespace tnrp::bin {

enum Tag : uint8_t {
    kTelemetry = 1,
    kMotion    = 2,
    kPositions = 3,
    kMotionEx  = 4,
};

inline constexpr uint8_t kRevLightsPercentUnavailable = UINT8_MAX;
inline constexpr uint16_t kRevLightsBitValueUnavailable = UINT16_MAX;

inline uint8_t rowTypeForTag(uint8_t tag) {
    switch (tag) {
        case kTelemetry: return 1;
        case kMotion: return 11;
        case kMotionEx: return 12;
        case kPositions: return 13;
        default: return 0;
    }
}

// Copies only selected complete records without decoding their fields. This is
// used by live and legacy playback subscriptions so hidden hot families never
// cross N-API/IPC or reach the renderer.
template <class F>
inline bool forEachPackedRecord(const uint8_t* data, size_t len, F&& callback) {
    size_t offset = 0;
    while (offset < len) {
        const uint8_t tag = data[offset];
        size_t recordLen = 0;
        switch (tag) {
            case kTelemetry: recordLen = 49; break;
            case kMotion: recordLen = 29; break;
            case kMotionEx: recordLen = 21; break;
            case kPositions:
                if (len - offset < 3) return false;
                recordLen = 3 + static_cast<size_t>(data[offset + 2]) * 16;
                break;
            default: return false;
        }
        if (recordLen > len - offset) return false;
        const uint8_t rowType = rowTypeForTag(tag);
        callback(rowType, data + offset, recordLen);
        offset += recordLen;
    }
    return true;
}

inline bool appendFilteredBatch(std::vector<uint8_t>& out, const uint8_t* data,
                                size_t len, uint32_t rowTypeMask) {
    return forEachPackedRecord(data, len,
        [&](uint8_t rowType, const uint8_t* record, size_t recordLen) {
            if (rowTypeMask & (1u << rowType))
                out.insert(out.end(), record, record + recordLen);
        });
}

template <class T>
inline void put(std::vector<uint8_t>& b, T v) {
    static_assert(std::is_trivially_copyable_v<T>, "POD only");
    const auto* p = reinterpret_cast<const uint8_t*>(&v);
    b.insert(b.end(), p, p + sizeof(T));
}

inline void encodeTelemetry(std::vector<uint8_t>& b, const TelemetryRow& t) {
    put<uint8_t>(b, kTelemetry);
    put<float>(b, t.session_time);
    put<uint16_t>(b, (uint16_t)t.speed_kph);
    put<uint16_t>(b, (uint16_t)t.rpm);
    put<int8_t>(b, (int8_t)t.gear);
    put<uint8_t>(b, (uint8_t)t.drs);
    put<uint8_t>(b, t.rev_lights_pct
        ? static_cast<uint8_t>(*t.rev_lights_pct)
        : kRevLightsPercentUnavailable);
    put<uint16_t>(b, t.rev_lights_bit_value
        ? static_cast<uint16_t>(*t.rev_lights_bit_value)
        : kRevLightsBitValueUnavailable);
    put<float>(b, t.throttle);
    put<float>(b, t.brake);
    put<double>(b, t.steering);
    put<uint8_t>(b, (uint8_t)t.tyre_temp_surface_rl);
    put<uint8_t>(b, (uint8_t)t.tyre_temp_surface_rr);
    put<uint8_t>(b, (uint8_t)t.tyre_temp_surface_fl);
    put<uint8_t>(b, (uint8_t)t.tyre_temp_surface_fr);
    put<uint8_t>(b, (uint8_t)t.tyre_temp_inner_rl);
    put<uint8_t>(b, (uint8_t)t.tyre_temp_inner_rr);
    put<uint8_t>(b, (uint8_t)t.tyre_temp_inner_fl);
    put<uint8_t>(b, (uint8_t)t.tyre_temp_inner_fr);
    put<uint16_t>(b, (uint16_t)t.brake_temp_rl);
    put<uint16_t>(b, (uint16_t)t.brake_temp_rr);
    put<uint16_t>(b, (uint16_t)t.brake_temp_fl);
    put<uint16_t>(b, (uint16_t)t.brake_temp_fr);
    put<uint16_t>(b, (uint16_t)t.engine_temp);
    put<uint8_t>(b, (uint8_t)t.slm);
}

inline void encodeMotion(std::vector<uint8_t>& b, const MotionRow& m) {
    put<uint8_t>(b, kMotion);
    put<float>(b, m.session_time);
    put<double>(b, m.g_lat);
    put<double>(b, m.g_long);
    put<double>(b, m.g_vert);
}

inline void encodeMotionEx(std::vector<uint8_t>& b, const MotionExRow& m) {
    put<uint8_t>(b, kMotionEx);
    put<float>(b, m.session_time);
    put<double>(b, m.front_aero_height_mm);
    put<double>(b, m.rear_aero_height_mm);
}

inline void encodePositions(std::vector<uint8_t>& b, const PositionsRow& p) {
    put<uint8_t>(b, kPositions);
    put<uint8_t>(b, (uint8_t)p.player_idx);
    put<uint8_t>(b, (uint8_t)p.cars.size());
    for (const auto& c : p.cars) {   // idx is implicit (== array index) on decode
        put<double>(b, c.x);
        put<double>(b, c.z);
    }
}

// ── Decode (C++ mirror of decodeBinaryBatch.ts, for in-process consumers) ───
// Walks a packed batch and invokes `f(row)` once per record with the typed row
// (TelemetryRow / MotionRow / MotionExRow / PositionsRow). Returns false and
// stops on a malformed/truncated batch. `ts` is left empty — the batch format
// deliberately omits it (see the encode notes above).

struct BinReader {
    const uint8_t* p;
    const uint8_t* end;
    template <class T>
    bool get(T& v) {
        static_assert(std::is_trivially_copyable_v<T>, "POD only");
        if (end - p < (ptrdiff_t)sizeof(T)) return false;
        std::memcpy(&v, p, sizeof(T));
        p += sizeof(T);
        return true;
    }
    // Reads a stored S and widens/converts it into the row's field type D.
    template <class S, class D>
    bool getAs(D& out) {
        S v{};
        if (!get(v)) return false;
        out = (D)v;
        return true;
    }
};

inline bool decodeTelemetry(BinReader& r, TelemetryRow& t) {
    uint8_t revLightsPercent = kRevLightsPercentUnavailable;
    uint16_t revLightsBitValue = kRevLightsBitValueUnavailable;
    if (!(r.get(t.session_time)
        && r.getAs<uint16_t>(t.speed_kph)
        && r.getAs<uint16_t>(t.rpm)
        && r.getAs<int8_t>(t.gear)
        && r.getAs<uint8_t>(t.drs)
        && r.get(revLightsPercent)
        && r.get(revLightsBitValue)
        && r.get(t.throttle)
        && r.get(t.brake)
        && r.get(t.steering)
        && r.getAs<uint8_t>(t.tyre_temp_surface_rl)
        && r.getAs<uint8_t>(t.tyre_temp_surface_rr)
        && r.getAs<uint8_t>(t.tyre_temp_surface_fl)
        && r.getAs<uint8_t>(t.tyre_temp_surface_fr)
        && r.getAs<uint8_t>(t.tyre_temp_inner_rl)
        && r.getAs<uint8_t>(t.tyre_temp_inner_rr)
        && r.getAs<uint8_t>(t.tyre_temp_inner_fl)
        && r.getAs<uint8_t>(t.tyre_temp_inner_fr)
        && r.getAs<uint16_t>(t.brake_temp_rl)
        && r.getAs<uint16_t>(t.brake_temp_rr)
        && r.getAs<uint16_t>(t.brake_temp_fl)
        && r.getAs<uint16_t>(t.brake_temp_fr)
        && r.getAs<uint16_t>(t.engine_temp)
        && r.getAs<uint8_t>(t.slm))) return false;

    if (revLightsPercent != kRevLightsPercentUnavailable)
        t.rev_lights_pct = revLightsPercent;
    else
        t.rev_lights_pct.reset();
    if (revLightsBitValue != kRevLightsBitValueUnavailable)
        t.rev_lights_bit_value = revLightsBitValue;
    else
        t.rev_lights_bit_value.reset();
    return true;
}

inline bool decodeMotion(BinReader& r, MotionRow& m) {
    return r.get(m.session_time) && r.get(m.g_lat) && r.get(m.g_long) && r.get(m.g_vert);
}

inline bool decodeMotionEx(BinReader& r, MotionExRow& m) {
    return r.get(m.session_time) && r.get(m.front_aero_height_mm) && r.get(m.rear_aero_height_mm);
}

inline bool decodePositions(BinReader& r, PositionsRow& p) {
    uint8_t count = 0;
    if (!r.getAs<uint8_t>(p.player_idx) || !r.get(count)) return false;
    p.cars.resize(count);
    for (int i = 0; i < (int)count; ++i) {
        p.cars[i].idx = i;   // implicit on the wire (== array index)
        if (!r.get(p.cars[i].x) || !r.get(p.cars[i].z)) return false;
    }
    return true;
}

template <class F>
inline bool decodeBatch(const uint8_t* data, size_t len, F&& f) {
    BinReader r{ data, data + len };
    while (r.p < r.end) {
        uint8_t tag = 0;
        if (!r.get(tag)) return false;
        switch (tag) {
            case kTelemetry: { TelemetryRow t; if (!decodeTelemetry(r, t)) return false; f(std::move(t)); break; }
            case kMotion:    { MotionRow m;    if (!decodeMotion(r, m))    return false; f(std::move(m)); break; }
            case kMotionEx:  { MotionExRow m;  if (!decodeMotionEx(r, m))  return false; f(std::move(m)); break; }
            case kPositions: { PositionsRow p; if (!decodePositions(r, p)) return false; f(std::move(p)); break; }
            default: return false;   // unknown tag — cannot resync, stop
        }
    }
    return true;
}

} // namespace tnrp::bin
