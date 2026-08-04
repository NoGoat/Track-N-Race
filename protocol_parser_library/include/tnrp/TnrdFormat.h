#pragma once

namespace tnrp {

// The on-disk TNRD generations. V1/V2 are retained for playback compatibility;
// all normal recording paths use V3. V2 and V3 both use Zstandard, so their
// JSON header magic (not the compression-frame signature) distinguishes them.
enum class TnrdFormat {
    Unknown,
    GzipV1,
    ZstdV2,
    ZstdV3,
};

inline bool isZstd(TnrdFormat format) {
    return format == TnrdFormat::ZstdV2 || format == TnrdFormat::ZstdV3;
}

inline const char* toString(TnrdFormat format) {
    switch (format) {
        case TnrdFormat::GzipV1: return "TNRD_V1/gzip";
        case TnrdFormat::ZstdV2: return "TNRD_V2/zstd";
        case TnrdFormat::ZstdV3: return "TNRD_V3/zstd";
        default:                 return "unknown";
    }
}

} // namespace tnrp
