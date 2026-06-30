#pragma once

#include <cstdint>
#include <type_traits>
#include <vector>

#include "tnrp/rows.h"

// Binary wire format for the hot 60 Hz rows (telemetry / motion / positions /
// motion_ex). These are all-numeric, so instead of serialising them to JSON,
// widening to UTF-16 across the N-API boundary and JSON.parse-ing them in the
// renderer, the engine packs them into fixed-layout little-endian records and
// ships the raw bytes. The renderer decodes them with a DataView (see
// src/renderer/src/lib/decodeBinaryBatch.ts — keep the two in sync).
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

} // namespace tnrp::bin
