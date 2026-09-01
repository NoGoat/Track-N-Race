#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <cmath>

struct PacketHeader {
    uint16_t packetFormat;
    uint8_t packetId;
    float sessionTime;
    uint32_t overallFrameId;
    uint8_t playerCarIndex;
};

// Side-channel output of ParsePacket for the hot 60 Hz rows. Cold (<=2 Hz) rows
// are still the function's return value (JSON, recorded + forwarded live). Hot
// rows go to `binary` (forwarded live as packed bytes) and, only when
// `wantHotJson` is set (i.e. logging is on), additionally to `hotJson` so they
// can be recorded in the same JSONL format as before.
struct HotOut {
    std::vector<std::string> hotJson;
    std::vector<uint8_t>     binary;
    bool                     wantHotJson = false;
    uint32_t                 outputRowMask = 0xFFFFFFFFu;
    bool wants(uint8_t rowType) const {
        return (outputRowMask & (1u << rowType)) != 0;
    }
};

// Binary reading helper functions
inline float ReadFloat(const uint8_t* data, int offset) {
    return *(const float*)(data + offset);
}
inline uint32_t ReadUInt32(const uint8_t* data, int offset) {
    return *(const uint32_t*)(data + offset);
}
inline uint16_t ReadUInt16(const uint8_t* data, int offset) {
    return *(const uint16_t*)(data + offset);
}
inline int16_t ReadInt16(const uint8_t* data, int offset) {
    return *(const int16_t*)(data + offset);
}
inline uint8_t ReadUInt8(const uint8_t* data, int offset) {
    return data[offset];
}
inline int8_t ReadInt8(const uint8_t* data, int offset) {
    return (int8_t)data[offset];
}

// Math rounding utilities matching JavaScript telemetry parser
inline double Round1(double v) {
    return std::round(v * 10.0) / 10.0;
}
inline double Round2(double v) {
    return std::round(v * 100.0) / 100.0;
}
inline double Round3(double v) {
    return std::round(v * 1000.0) / 1000.0;
}
inline double Round4(double v) {
    return std::round(v * 10000.0) / 10000.0;
}

std::string ReadString(const uint8_t* data, int offset, int length);

extern const std::unordered_map<int, std::string> TRACK_NAMES;
extern const std::unordered_map<int, std::string> SESSION_NAMES;

// Recording filenames are protocol-owned. Callers use this generic dispatch
// instead of duplicating format-to-prefix knowledge outside the protocol layer.
std::string RecordingFilenamePrefix(uint16_t format);
