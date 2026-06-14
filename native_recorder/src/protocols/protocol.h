#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <cmath>
#include <nlohmann/json.hpp>

struct PacketHeader {
    uint16_t packetFormat;
    uint8_t packetId;
    float sessionTime;
    uint32_t overallFrameId;
    uint8_t playerCarIndex;
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
