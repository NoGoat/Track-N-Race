#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

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

    // Packed binary batch for the hot 60 Hz rows (see tnrp/BinaryRows.h). Only the
    // live UDP path emits these; playback rows arrive as JSON via onRow(). Default
    // no-op so sinks that don't care (e.g. a JSON-only pipe) need not implement it.
    virtual void onBinary(const uint8_t* /*data*/, size_t /*len*/) {}
};

} // namespace tnrp
