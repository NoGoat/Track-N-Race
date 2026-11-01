#pragma once

#include <vector>

#include "tnrp/LapDelta.h"

namespace tnrp::detail {

// A lap's cleaned distance/elapsed-time curve: the timed lap only (any out-lap
// prefix before a stale timer reset is dropped), anchored at its origin, with
// time and distance never decreasing. Shared by the lap delta and the
// Analysis map calculations so both read a lap the same way.
struct ProgressMap {
    std::vector<LapProgressPoint> points;
    // Elapsed time at each point, in ms: current_lap_ms, aligned with the lap's
    // official split times by the lap delta when it needs them.
    std::vector<double> elapsedMs;
    float maxDistance{};
};

ProgressMap buildProgressMap(const AnalysisLapProgress& lap);

// Elapsed seconds at a lap distance; NaN outside the recorded distances.
double interpolateElapsed(const ProgressMap& progress, float distance);

} // namespace tnrp::detail
