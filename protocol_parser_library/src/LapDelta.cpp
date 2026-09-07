#include "tnrp/LapDelta.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>

namespace tnrp {
namespace {

struct ProgressMap {
    std::vector<LapProgressPoint> points;
    float maxDistance{};
};

ProgressMap buildProgressMap(const AnalysisLapProgress& lap) {
    ProgressMap result;
    if (lap.points.empty()) return result;

    float originDistance = std::numeric_limits<float>::infinity();
    for (const auto& point : lap.points) {
        if (point.session_time > lap.endSessionTime) break;
        if (point.session_time < lap.startSessionTime || point.current_lap_ms != 0 ||
            !std::isfinite(point.lap_distance_m) || point.lap_distance_m < 0.0f) continue;
        originDistance = std::min(originDistance, point.lap_distance_m);
    }
    if (!std::isfinite(originDistance)) originDistance = 0.0f;

    LapProgressPoint origin;
    origin.session_time = lap.startSessionTime;
    origin.current_lap_ms = 0;
    origin.lap_distance_m = originDistance;
    origin.sector = 0;
    result.points.push_back(origin);

    float lastTime = lap.startSessionTime;
    float lastDistance = originDistance;
    for (const auto& point : lap.points) {
        if (point.session_time > lap.endSessionTime) break;
        if (!std::isfinite(point.session_time) || !std::isfinite(point.lap_distance_m) ||
            point.session_time < lastTime || point.lap_distance_m < lastDistance ||
            point.current_lap_ms < 0) continue;
        if (point.session_time == lap.startSessionTime) continue;
        if (result.points.size() == 1) {
            if (point.lap_distance_m == originDistance) continue;
            result.points.push_back(point);
        } else if (point.lap_distance_m == lastDistance) {
            continue;
        } else if (point.session_time == lastTime) {
            result.points.back() = point;
        } else {
            result.points.push_back(point);
        }
        lastTime = point.session_time;
        lastDistance = point.lap_distance_m;
    }
    if (result.points.size() < 2) {
        result.points.clear();
        return result;
    }
    result.maxDistance = lastDistance;
    return result;
}

double interpolateElapsed(const ProgressMap& progress, float distance) {
    if (progress.points.empty() || distance < progress.points.front().lap_distance_m ||
        distance > progress.maxDistance) return std::numeric_limits<double>::quiet_NaN();
    const auto after = std::lower_bound(
        std::next(progress.points.begin()), progress.points.end(), distance,
        [](const LapProgressPoint& point, float value) {
            return point.lap_distance_m < value;
        });
    if (after == progress.points.end())
        return static_cast<double>(progress.points.back().current_lap_ms) / 1000.0;
    const auto& before = *std::prev(after);
    const float span = after->lap_distance_m - before.lap_distance_m;
    const double ratio = span > 0.0f
        ? static_cast<double>(distance - before.lap_distance_m) / span
        : 1.0;
    return (before.current_lap_ms +
        (after->current_lap_ms - before.current_lap_ms) * ratio) / 1000.0;
}

void appendValid(LapDeltaResult& result, float distance, double delta) {
    if (!std::isfinite(delta)) return;
    result.samples.push_back({distance, delta, true});
    result.maxAbsDeltaSeconds = std::max(result.maxAbsDeltaSeconds, std::abs(delta));
}

} // namespace

LapDeltaResult calculateLapDelta(const AnalysisLapProgress& current,
                                 const AnalysisLapProgress& comparison,
                                 bool sectorDelta) {
    LapDeltaResult result;
    result.currentLapNum = current.lapNum;
    result.comparisonLapNum = comparison.lapNum;
    result.sectorDelta = sectorDelta;

    const ProgressMap currentMap = buildProgressMap(current);
    const ProgressMap comparisonMap = buildProgressMap(comparison);
    if (currentMap.points.empty() || comparisonMap.points.empty()) return result;

    std::vector<float> sectorStarts;
    if (sectorDelta) {
        const float sector1 = current.sector1EndDistanceM > 0.0f
            ? current.sector1EndDistanceM : comparison.sector1EndDistanceM;
        const float sector2 = current.sector2EndDistanceM > 0.0f
            ? current.sector2EndDistanceM : comparison.sector2EndDistanceM;
        if (sector1 > 0.0f) sectorStarts.push_back(sector1);
        if (sector2 > sector1 && sector2 > 0.0f) sectorStarts.push_back(sector2);
    }

    const float maxDistance = std::min(currentMap.maxDistance, comparisonMap.maxDistance);
    float lastStoredDistance = -std::numeric_limits<float>::infinity();
    for (const auto& point : currentMap.points) {
        const float distance = point.lap_distance_m;
        if (distance > maxDistance) break;

        double currentBase = 0.0;
        double comparisonBase = 0.0;
        if (sectorDelta) {
            float sectorStart = 0.0f;
            for (const float boundary : sectorStarts) {
                if (boundary > distance) break;
                sectorStart = boundary;
            }
            if (sectorStart > 0.0f) {
                currentBase = interpolateElapsed(currentMap, sectorStart);
                comparisonBase = interpolateElapsed(comparisonMap, sectorStart);
            }
        }

        const double delta =
            (interpolateElapsed(currentMap, distance) - currentBase) -
            (interpolateElapsed(comparisonMap, distance) - comparisonBase);
        if (!std::isfinite(delta)) continue;

        if (sectorDelta) {
            for (const float boundary : sectorStarts) {
                if (boundary <= lastStoredDistance || boundary > distance) continue;
                float previousBoundary = 0.0f;
                for (const float prior : sectorStarts) {
                    if (prior >= boundary) break;
                    previousBoundary = prior;
                }
                const double previousCurrentBase = previousBoundary > 0.0f
                    ? interpolateElapsed(currentMap, previousBoundary) : 0.0;
                const double previousComparisonBase = previousBoundary > 0.0f
                    ? interpolateElapsed(comparisonMap, previousBoundary) : 0.0;
                const double completedSectorDelta =
                    (interpolateElapsed(currentMap, boundary) - previousCurrentBase) -
                    (interpolateElapsed(comparisonMap, boundary) - previousComparisonBase);
                appendValid(result, boundary, completedSectorDelta);
                result.samples.push_back({boundary, 0.0, false});
                result.samples.push_back({boundary, 0.0, true});
                lastStoredDistance = boundary;
            }
        }
        appendValid(result, distance, delta);
        lastStoredDistance = distance;
    }
    return result;
}

} // namespace tnrp
