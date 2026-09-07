#pragma once

#include <vector>

#include "tnrp/control_rows.h"

namespace tnrp {

// Complete, immutable progress data for one recorded lap. Hosts identify the
// laps to compare; libtnrp owns cleaning, interpolation and delta semantics.
struct AnalysisLapProgress {
    int lapNum{};
    float startSessionTime{};
    float endSessionTime{};
    float sector1EndDistanceM{};
    float sector2EndDistanceM{};
    std::vector<LapProgressPoint> points;
};

struct LapDeltaSample {
    float lap_distance_m{};
    double delta_seconds{};
    // False marks the discontinuity between completed-sector delta and the
    // next sector's zero baseline. JSON cannot represent a NaN sentinel.
    bool valid{true};
};

struct LapDeltaResult {
    int currentLapNum{};
    int comparisonLapNum{};
    bool sectorDelta{};
    double maxAbsDeltaSeconds{};
    std::vector<LapDeltaSample> samples;
};

LapDeltaResult calculateLapDelta(const AnalysisLapProgress& current,
                                 const AnalysisLapProgress& comparison,
                                 bool sectorDelta);

} // namespace tnrp
