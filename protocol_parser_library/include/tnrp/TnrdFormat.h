#pragma once

namespace tnrp {

// The on-disk TNRD generations. V1/V2 are retained for playback compatibility;
// normal recording uses indexed V5. V2 and V3 both use monolithic Zstandard, so their
// JSON header magic (not the compression-frame signature) distinguishes them.
enum class TnrdFormat {
    Unknown,
    GzipV1,
    ZstdV2,
    ZstdV3,
    ChunkedV4,
    ChunkedV5,
};

inline bool isLegacyZstdStream(TnrdFormat format) {
    return format == TnrdFormat::ZstdV2 || format == TnrdFormat::ZstdV3;
}
inline bool isChunkedTnrd(TnrdFormat format) {
    return format == TnrdFormat::ChunkedV4 || format == TnrdFormat::ChunkedV5;
}
inline bool usesZstdCompression(TnrdFormat format) { return isLegacyZstdStream(format) || isChunkedTnrd(format); }
inline bool isZstd(TnrdFormat format) { return isLegacyZstdStream(format); }

inline const char* toString(TnrdFormat format) {
    switch (format) {
        case TnrdFormat::GzipV1: return "TNRD_V1/gzip";
        case TnrdFormat::ZstdV2: return "TNRD_V2/zstd";
        case TnrdFormat::ZstdV3: return "TNRD_V3/zstd";
        case TnrdFormat::ChunkedV4: return "TNRD_V4/chunked-zstd";
        case TnrdFormat::ChunkedV5: return "TNRD_V5/chunked-zstd";
        default:                 return "unknown";
    }
}

} // namespace tnrp
