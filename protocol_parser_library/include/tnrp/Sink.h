#pragma once

#include <nlohmann/json.hpp>

namespace tnrp {

// The single seam between the engine and its consumers. The engine pushes every
// parsed telemetry/control row through onRow(); the consumer decides what to do
// with it. The headless bridge implements this by writing one JSON line to a
// named pipe / domain socket; the native Qt app will later implement it by
// emitting its existing Qt signals. Neither requires changing the engine.
//
// onRow() may be called from the engine's UDP receive thread or its playback
// thread, so implementations must be thread-safe.
class Sink {
public:
    virtual ~Sink() = default;
    virtual void onRow(const nlohmann::json& row) = 0;
};

} // namespace tnrp
