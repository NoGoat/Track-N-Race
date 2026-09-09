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

float resolvedDistance(float current, float comparison) {
    return current > 0.0f ? current : comparison;
}

bool exactCumulativeElapsed(const AnalysisLapProgress& lap,
                            int completedSectors,
                            double& secondsOut) {
    int milliseconds = 0;
    if (completedSectors == 1 && lap.sector1TimeMs > 0) {
        milliseconds = lap.sector1TimeMs;
    } else if (completedSectors == 2 && lap.sector1TimeMs > 0 && lap.sector2TimeMs > 0) {
        milliseconds = lap.sector1TimeMs + lap.sector2TimeMs;
    } else if (completedSectors == 3 && lap.lapTimeMs > 0) {
        milliseconds = lap.lapTimeMs;
    } else {
        return false;
    }
    secondsOut = static_cast<double>(milliseconds) / 1000.0;
    return true;
}

bool exactCumulativeDelta(const AnalysisLapProgress& current,
                          const AnalysisLapProgress& comparison,
                          int completedSectors,
                          double& deltaOut) {
    double currentElapsed = 0.0;
    double comparisonElapsed = 0.0;
    if (!exactCumulativeElapsed(current, completedSectors, currentElapsed) ||
        !exactCumulativeElapsed(comparison, completedSectors, comparisonElapsed)) return false;
    deltaOut = currentElapsed - comparisonElapsed;
    return true;
}

bool exactSectorDelta(const AnalysisLapProgress& current,
                      const AnalysisLapProgress& comparison,
                      int sectorIndex,
                      double& deltaOut) {
    int currentMs = 0;
    int comparisonMs = 0;
    if (sectorIndex == 0 && current.sector1TimeMs > 0 && comparison.sector1TimeMs > 0) {
        currentMs = current.sector1TimeMs;
        comparisonMs = comparison.sector1TimeMs;
    } else if (sectorIndex == 1 && current.sector2TimeMs > 0 && comparison.sector2TimeMs > 0) {
        currentMs = current.sector2TimeMs;
        comparisonMs = comparison.sector2TimeMs;
    } else if (sectorIndex == 2 &&
               current.lapTimeMs > current.sector1TimeMs + current.sector2TimeMs &&
               comparison.lapTimeMs > comparison.sector1TimeMs + comparison.sector2TimeMs &&
               current.sector1TimeMs > 0 && current.sector2TimeMs > 0 &&
               comparison.sector1TimeMs > 0 && comparison.sector2TimeMs > 0) {
        currentMs = current.lapTimeMs - current.sector1TimeMs - current.sector2TimeMs;
        comparisonMs = comparison.lapTimeMs - comparison.sector1TimeMs - comparison.sector2TimeMs;
    } else {
        return false;
    }
    deltaOut = static_cast<double>(currentMs - comparisonMs) / 1000.0;
    return true;
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
    const float sector1 = resolvedDistance(
        current.sector1EndDistanceM, comparison.sector1EndDistanceM);
    const float sector2 = resolvedDistance(
        current.sector2EndDistanceM, comparison.sector2EndDistanceM);
    if (sector1 > 0.0f) sectorStarts.push_back(sector1);
    if (sector1 > 0.0f && sector2 > sector1) sectorStarts.push_back(sector2);

    const float finishDistance = resolvedDistance(current.trackLengthM, comparison.trackLengthM);
    double exactFinishDelta = 0.0;
    const bool hasExactFinish = finishDistance > 0.0f &&
        (sectorStarts.empty() || finishDistance > sectorStarts.back()) &&
        exactCumulativeDelta(current, comparison, 3, exactFinishDelta);

    float maxDistance = std::min(currentMap.maxDistance, comparisonMap.maxDistance);
    if (hasExactFinish) maxDistance = std::min(maxDistance, finishDistance);
    float lastStoredDistance = -std::numeric_limits<float>::infinity();
    size_t nextCumulativeAnchor = 0;
    for (const auto& point : currentMap.points) {
        const float distance = point.lap_distance_m;
        if (distance > maxDistance) break;
        // Leave the exact timing-line value as the sole finish sample. Packet
        // positions can straddle the line differently on otherwise equal laps.
        if (hasExactFinish && distance >= finishDistance) break;

        double currentBase = 0.0;
        double comparisonBase = 0.0;
        if (sectorDelta) {
            for (size_t index = 0; index < sectorStarts.size(); ++index) {
                const float boundary = sectorStarts[index];
                if (boundary > distance) break;
                if (!exactCumulativeElapsed(current, static_cast<int>(index + 1), currentBase))
                    currentBase = interpolateElapsed(currentMap, boundary);
                if (!exactCumulativeElapsed(comparison, static_cast<int>(index + 1), comparisonBase))
                    comparisonBase = interpolateElapsed(comparisonMap, boundary);
            }
        }

        const double delta =
            (interpolateElapsed(currentMap, distance) - currentBase) -
            (interpolateElapsed(comparisonMap, distance) - comparisonBase);
        if (!std::isfinite(delta)) continue;

        if (sectorDelta) {
            for (size_t index = 0; index < sectorStarts.size(); ++index) {
                const float boundary = sectorStarts[index];
                if (boundary <= lastStoredDistance || boundary > distance) continue;
                float previousBoundary = 0.0f;
                if (index > 0) previousBoundary = sectorStarts[index - 1];
                double completedSectorDelta = 0.0;
                if (!exactSectorDelta(current, comparison, static_cast<int>(index),
                                      completedSectorDelta)) {
                    const double previousCurrentBase = previousBoundary > 0.0f
                        ? interpolateElapsed(currentMap, previousBoundary) : 0.0;
                    const double previousComparisonBase = previousBoundary > 0.0f
                        ? interpolateElapsed(comparisonMap, previousBoundary) : 0.0;
                    completedSectorDelta =
                        (interpolateElapsed(currentMap, boundary) - previousCurrentBase) -
                        (interpolateElapsed(comparisonMap, boundary) - previousComparisonBase);
                }
                appendValid(result, boundary, completedSectorDelta);
                result.samples.push_back({boundary, 0.0, false});
                result.samples.push_back({boundary, 0.0, true});
                lastStoredDistance = boundary;
            }
        } else {
            while (nextCumulativeAnchor < sectorStarts.size() &&
                   sectorStarts[nextCumulativeAnchor] <= distance) {
                const float boundary = sectorStarts[nextCumulativeAnchor];
                double exactDelta = 0.0;
                if (boundary > lastStoredDistance &&
                    exactCumulativeDelta(current, comparison,
                                         static_cast<int>(nextCumulativeAnchor + 1), exactDelta)) {
                    appendValid(result, boundary, exactDelta);
                    lastStoredDistance = boundary;
                }
                ++nextCumulativeAnchor;
            }
        }
        if (distance <= lastStoredDistance) continue;
        appendValid(result, distance, delta);
        lastStoredDistance = distance;
    }

    // A completed lap can still have its final sampled point before a sector
    // line. Preserve every remaining authoritative split before the finish.
    if (hasExactFinish) {
        while (nextCumulativeAnchor < sectorStarts.size()) {
            const float boundary = sectorStarts[nextCumulativeAnchor];
            if (boundary > lastStoredDistance) {
                double exactDelta = 0.0;
                if (sectorDelta) {
                    if (exactSectorDelta(current, comparison,
                                         static_cast<int>(nextCumulativeAnchor), exactDelta)) {
                        appendValid(result, boundary, exactDelta);
                        result.samples.push_back({boundary, 0.0, false});
                        result.samples.push_back({boundary, 0.0, true});
                        lastStoredDistance = boundary;
                    }
                } else if (exactCumulativeDelta(
                               current, comparison,
                               static_cast<int>(nextCumulativeAnchor + 1), exactDelta)) {
                    appendValid(result, boundary, exactDelta);
                    lastStoredDistance = boundary;
                }
            }
            ++nextCumulativeAnchor;
        }
    }

    // Completed laps have an authoritative timing-line endpoint even though
    // their final menu-rate position samples generally land on opposite sides
    // of that line. Anchor the curve there so its final value equals the lap-
    // time difference rather than the last coincident packet position.
    if (hasExactFinish && finishDistance > lastStoredDistance) {
        if (sectorDelta) {
            double finalSectorDelta = exactFinishDelta;
            double completedSectorDelta = 0.0;
            const bool haveCompletedSectorDelta = sectorStarts.empty() ||
                exactCumulativeDelta(current, comparison,
                                     static_cast<int>(sectorStarts.size()), completedSectorDelta);
            if (haveCompletedSectorDelta) {
                finalSectorDelta -= completedSectorDelta;
                appendValid(result, finishDistance, finalSectorDelta);
            }
        } else {
            appendValid(result, finishDistance, exactFinishDelta);
        }
    }
    return result;
}

} // namespace tnrp
