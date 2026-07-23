#pragma once

namespace tnrp {

// The on-disk TNRD generations. V1 is retained for compatibility; all normal
// recording paths use V2.
enum class TnrdFormat {
    Unknown,
    GzipV1,
    ZstdV2,
};

inline const char* toString(TnrdFormat format) {
    switch (format) {
        case TnrdFormat::GzipV1: return "TNRD_V1/gzip";
        case TnrdFormat::ZstdV2: return "TNRD_V2/zstd";
        default:                 return "unknown";
    }
}

} // namespace tnrp
