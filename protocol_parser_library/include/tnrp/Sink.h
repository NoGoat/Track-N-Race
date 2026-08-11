#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace tnrp {

// The single seam between the engine and its consumers. The engine pushes every
// parsed telemetry/control row through onRow(); the consumer decides what to do
// with it. Rows arrive as pre-serialised JSON strings — no further conversion
// is needed on the consumer side.
//
// onRow() may be called from the engine's UDP receive thread or its playback
// thread, so implementations must be thread-safe.
class Sink {
public:
    virtual ~Sink() = default;
    virtual void onRow(const std::string& json) = 0;

    // Packed binary batch for the hot 60 Hz rows (see tnrp/BinaryRows.h). The
    // live UDP path emits these; playback does too when the engine runs with
    // Config::binaryPlayback. Default no-op so sinks that don't care (e.g. a
    // JSON-only pipe) need not implement it.
    virtual void onBinary(const uint8_t* /*data*/, size_t /*len*/) {}

    // Seek flush under Config::binaryPlayback: the hot rows of the window
    // leading up to the seek target as one packed binary slice, plus the sparse
    // cold rows (status/damage/lap) of the same window as newline-joined JSON.
    // allHistory marks AL's session-start-to-playhead prefix payload.
    // JSON-only consumers never receive this (legacy mode emits a
    // playback_seek_flush row via onRow instead).
    virtual void onSeekFlush(std::shared_ptr<const std::vector<uint8_t>> /*binStore*/,
                             size_t /*binBegin*/, size_t /*binEnd*/,
                             std::string&& /*coldJson*/,
                             float /*currentLapStart*/, int /*lapNum*/,
                             bool /*allHistory*/ = false,
                             uint64_t /*requestId*/ = 0,
                             bool /*authoritativeSeek*/ = true) {}
};

} // namespace tnrp
