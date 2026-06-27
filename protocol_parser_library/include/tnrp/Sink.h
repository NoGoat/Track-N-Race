#pragma once

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
};

} // namespace tnrp
