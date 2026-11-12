#include "tnrp/AnalysisMap.h"

#include "LapProgressMap.h"
#include "tnrp/TnrdReader.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <limits>
#include <map>
#include <numbers>
#include <optional>
#include <tuple>
#include <utility>

namespace tnrp {
namespace {

// Track Dominance compares the laps over mini-sectors of this length.
constexpr float kMiniSectorM = 100.0f;
// Superclipping: flat out (kFullThrottle) with the MGU-K deploying under
// kSuperclipDeployKw for at least kSuperclipShare of a bin's time, while
// harvesting at least kSuperclipHarvestKw there, over runs of at least
// kSuperclipMinBins bins. At Spa it holds a steady 20-24 kW down the Kemmel
// straight, against 350 kW under braking.
constexpr float kSuperclipDeployKw = 5.0f;
constexpr double kSuperclipShare = 0.5;
constexpr double kSuperclipHarvestKw = 10.0;
constexpr size_t kSuperclipMinBins = 3;
// Peak harvest power is measured over at least this long: the counters step
// in increments, so a single packet interval can read a spike.
constexpr float kHarvestWindowS = 0.25f;
// Driver inputs are classified over bins of this length.
constexpr float kInputBinM = 10.0f;
// Speed is averaged over bins of this length.
constexpr float kSpeedBinM = 10.0f;
// Pedal travel (0-1) that counts as pressed, and throttle that counts as flat out.
constexpr float kPedalPressed = 0.05f;
constexpr float kFullThrottle = 0.98f;
// Brake presses less than kBrakeOffBeforeS apart are one application: ABS
// pulses the brake channel through a stop, and pressure modulation dips it
// (at COTA, T1 on one lap went 16% for 0.19 s, under 5% for 0.08 s, then back
// on), and neither may split, shorten or drop a braking zone. An application
// is a braking zone when it lasts at least kBrakeHoldS.
constexpr float kBrakeOffBeforeS = 1.0f;
constexpr float kBrakeHoldS = 0.2f;
// Braking points this close on two laps are the same corner.
constexpr float kBrakingPairM = 100.0f;
// A zone splits into braking and acceleration at its laps' slowest points when
// they are this close, and only when each half is at least kPhaseMinM long.
constexpr float kPhaseAgreeM = 80.0f;
constexpr float kPhaseMinM = 30.0f;
// Coasting shorter than this before the brake is just the foot moving across.
constexpr float kMinCoastS = 0.1f;
// Observation thresholds: differences smaller than these are not worth saying.
constexpr float kObserveCoastS = 0.1f;
constexpr float kObserveApexKph = 3.0f;
constexpr float kObserveLaterM = 3.0f;
constexpr float kObserveHarvestKj = 20.0f;
constexpr float kObserveHarvestShare = 0.05f;
constexpr float kLowBatteryMj = 0.2f;
constexpr float kObserveExitM = 5.0f;
constexpr float kObserveLevelS = 0.01f;
constexpr size_t kMaxBrakingObservations = 4;
// Lines at a corner. Closer than kLineLevelM counts as the same line; a verdict
// needs kObserveLineM. The bend's inside is the way the corner turns, square
// to the lap's path across kLineTangentM either side of the point. A corner
// found from speed alone has no side: its inside comes from the lap's path
// kLineChordM either side of the apex, and is not trusted below kLineMinTurn
// (the two unit tangents' difference: about 3 degrees of turn). The other lap
// is searched kLineSearchM either side, and is too far away to compare beyond
// kLineMaxGapM. Apexes this close on two laps are the same corner.
constexpr float kLineLevelM = 0.5f;
constexpr float kObserveLineM = 1.0f;
constexpr float kLineTangentM = 5.0f;
constexpr float kLineChordM = 25.0f;
constexpr double kLineMinTurn = 0.05;
constexpr float kLineSearchM = 60.0f;
constexpr double kLineMaxGapM = 30.0;
constexpr float kLineCornerMatchM = 80.0f;
// A lap's own radius through a corner has to be this much larger than the
// other lap's to say it ran the wider arc. Not yet checked against recordings.
constexpr float kObserveRadiusShare = 0.05f;
// A track-limits penalty event can follow the corner it is for; it counts for
// a zone up to this long after the zone ends.
constexpr float kTrackLimitLagS = 2.0f;
// Wheelspin: in one gear, engine speed over road speed is fixed while the
// rear tyres grip. Each gear's ratio is the median over the lap's on-throttle,
// off-brake samples above kRatioMinKph (at least kRatioMinSamples of them),
// which covers 2nd gear where nothing is flat out for long. A corner exit
// spins when the engine runs kWheelspinSlip or more ahead of that for
// kWheelspinMinS; samples within kGearSettleS of a gear change are skipped.
// At COTA ordinary exits read 2-5% over the ratio (cornering, measurement),
// which kWheelspinSlip stays clear of.
constexpr float kRatioMinKph = 20.0f;
constexpr size_t kRatioMinSamples = 10;
constexpr float kWheelspinSlip = 0.07f;
constexpr float kWheelspinMinS = 0.15f;
constexpr float kGearSettleS = 0.15f;
// A corner exit is followed this long past full throttle for wheelspin.
constexpr float kExitTailS = 1.0f;
// A corner's apex is a dip in speed at least this deep on both sides. Only
// used for recordings without steering.
constexpr float kApexProminenceKph = 10.0f;
// Corners from the steering, smoothed over kSteerSmoothS: a corner starts when
// the wheel is turned kCornerOnSteer or more one way and ends below
// kCornerOffSteer or when it turns the other way; turns the same way split by
// less than kCornerJoinS are one corner, and a corner lasts kCornerMinS. Over
// 19 recorded circuits this gave each lap's corner count within about two of
// the official one, and steady from lap to lap.
constexpr float kSteerSmoothS = 0.5f;
constexpr float kCornerOnSteer = 0.05f;
constexpr float kCornerOffSteer = 0.025f;
constexpr float kCornerJoinS = 0.4f;
constexpr float kCornerMinS = 0.3f;
// Opposite lock shorter than this between two turns the same way is a
// correction (catching a slide, as on a wet São Paulo lap through Curva do
// Sol), not a corner of its own. Over the same circuits it only ever caught
// small mid-corner catches, and kept lap-to-lap counts steadier.
constexpr float kCorrectionMaxS = 0.7f;
// Where only one lap brakes, the other lap's lift is looked for from this far
// before the braking zone; it counts when off the throttle this long or when
// it costs at least kApexProminenceKph.
constexpr float kLiftLookbackM = 150.0f;
constexpr float kLiftMinS = 0.3f;
// Track corners (TrackPathAverage): every clean lap's path is averaged metre
// by metre of lap distance; its heading is taken across kPathHeadingM and its
// curvature across kPathCurvatureM. A corner is where that path turns tighter
// than kTrackCornerRadiusM for at least kTrackCornerMinM, and runs on either
// side while tighter than kTrackCornerEndRadiusM. Its apex is halfway through
// the turning of its core, where it turns at least kApexCoreShare as tightly
// as at its tightest: many corners hold one radius for a long stretch (São
// Paulo's Descida do Lago for 70 m), and the tightest point along it moved
// from one end to the other between one car's laps and every car's. Same-way corners less than kTrackCornerJoinM apart
// are one. Positions jumping more than kPathMaxStepM between samples are a
// teleport, and a lap needs kPathMinSamples of them; gaps in the averaged
// path up to kPathMaxGapM are bridged, and kPathMinLaps laps are needed. At
// São Paulo one car's laps and every car's put each braking corner's apex
// within 3 m of each other.
constexpr int kPathHeadingM = 5;
constexpr int kPathCurvatureM = 10;
constexpr double kTrackCornerRadiusM = 250.0;
constexpr double kTrackCornerEndRadiusM = 500.0;
constexpr double kApexCoreShare = 0.5;
constexpr int kTrackCornerMinM = 15;
constexpr int kTrackCornerJoinM = 20;
constexpr double kPathMaxStepM = 60.0;
constexpr size_t kPathMinSamples = 100;
constexpr int kPathMaxGapM = 20;
constexpr size_t kPathMinLaps = 3;
// ERS Usage is averaged over bins of this length.
constexpr float kErsBinM = 10.0f;
// Peaks closer than this are the same cap read with reporting noise: F1 25
// cars report up to 121.9 kW against their 120 kW cap.
constexpr float kPeakMismatchKw = 5.0f;

// Fills each empty entry from the nearest earlier one, wrapping past the line
// so the entries just after it take the end of the lap's value.
template <typename T>
void fillGaps(std::vector<std::optional<T>>& values) {
    std::optional<T> last;
    for (auto it = values.rbegin(); it != values.rend() && !last; ++it) last = *it;
    if (!last) return;
    for (auto& value : values) {
        if (value) last = value;
        else value = last;
    }
}

float lapLength(const AnalysisLapProgress& lap, float maxDistance) {
    return lap.trackLengthM > 0.0f ? lap.trackLengthM : maxDistance;
}

// Lap distance at a session time along the cleaned progress curve; NaN
// outside it.
float distanceAtTime(const detail::ProgressMap& progress, float sessionTime) {
    const auto& points = progress.points;
    if (points.size() < 2 || sessionTime < points.front().session_time ||
        sessionTime > points.back().session_time) return std::numeric_limits<float>::quiet_NaN();
    const auto after = std::lower_bound(
        std::next(points.begin()), points.end(), sessionTime,
        [](const LapProgressPoint& point, float value) { return point.session_time < value; });
    if (after == points.end()) return points.back().lap_distance_m;
    const auto& before = *std::prev(after);
    const float span = after->session_time - before.session_time;
    const float ratio = span > 0.0f ? (sessionTime - before.session_time) / span : 1.0f;
    return before.lap_distance_m + (after->lap_distance_m - before.lap_distance_m) * ratio;
}

bool isRestricted(const std::vector<ErsSample>& samples) {
    return std::all_of(samples.begin(), samples.end(), [](const ErsSample& sample) {
        return sample.mguk_kw == 0.0f && !(sample.ers_j > 0.0);
    });
}

struct BrakeApplication {
    size_t start;      // sample where the brake first goes on
    float startTime;
    float endTime;     // when it finally comes off
};

// The lap's brake applications, in order: presses merged across every gap
// shorter than kBrakeOffBeforeS, so applications are always at least that far
// apart.
std::vector<BrakeApplication> brakeApplications(const AnalysisLapProgress& lap,
                                                const std::vector<InputSample>& samples) {
    std::vector<BrakeApplication> applications;
    bool pressed = false;
    for (size_t i = 0; i < samples.size(); ++i) {
        const auto& sample = samples[i];
        if (sample.session_time < lap.startSessionTime || sample.session_time > lap.endSessionTime) continue;
        const bool now = sample.brake > kPedalPressed;
        if (now && !pressed) {
            if (!applications.empty() && sample.session_time - applications.back().endTime < kBrakeOffBeforeS)
                applications.back().endTime = lap.endSessionTime;   // the same application resumes
            else
                applications.push_back({i, sample.session_time, lap.endSessionTime});
        } else if (!now && pressed) {
            applications.back().endTime = sample.session_time;
        }
        pressed = now;
    }
    return applications;
}

// The lap's corners from its steering, as sample index ranges: smoothed over
// kSteerSmoothS, with hysteresis between kCornerOnSteer and kCornerOffSteer.
// Empty when the recording carries no steering.
struct SteeringCorner { size_t first; size_t last; int side; };
std::vector<SteeringCorner> steeringCorners(const std::vector<InputSample>& samples) {
    std::vector<SteeringCorner> corners;
    const bool any = std::any_of(samples.begin(), samples.end(),
        [](const InputSample& sample) { return sample.steering != 0.0f; });
    if (!any || samples.empty()) return corners;
    // Centred moving average over kSteerSmoothS, from prefix sums.
    std::vector<double> prefix(samples.size() + 1, 0.0);
    for (size_t i = 0; i < samples.size(); ++i) prefix[i + 1] = prefix[i] + samples[i].steering;
    std::vector<float> smooth(samples.size());
    size_t lo = 0, hi = 0;
    for (size_t i = 0; i < samples.size(); ++i) {
        const float t = samples[i].session_time;
        while (lo < i && samples[lo].session_time < t - kSteerSmoothS / 2.0f) ++lo;
        while (hi + 1 < samples.size() && samples[hi + 1].session_time <= t + kSteerSmoothS / 2.0f) ++hi;
        if (hi < i) hi = i;
        smooth[i] = static_cast<float>((prefix[hi + 1] - prefix[lo]) / static_cast<double>(hi + 1 - lo));
    }
    std::optional<SteeringCorner> current;
    for (size_t i = 0; i < samples.size(); ++i) {
        const int side = smooth[i] > 0.0f ? 1 : -1;
        const float amount = std::abs(smooth[i]);
        if (!current) {
            if (amount >= kCornerOnSteer) current = SteeringCorner{i, i, side};
        } else if (side == current->side && amount >= kCornerOffSteer) {
            current->last = i;
        } else {
            corners.push_back(*current);
            current.reset();
            if (amount >= kCornerOnSteer) current = SteeringCorner{i, i, side};
        }
    }
    if (current) corners.push_back(*current);
    // Same-way turns with only a short straightening between are one corner,
    // a brief catch of opposite lock does not split one, and a corner has to
    // last.
    std::vector<SteeringCorner> joined;
    for (const auto& corner : corners) {
        if (!joined.empty() && joined.back().side == corner.side &&
            samples[corner.first].session_time - samples[joined.back().last].session_time < kCornerJoinS)
            joined.back().last = corner.last;
        else
            joined.push_back(corner);
    }
    // A short opposite-lock run between two turns the other way is a
    // correction: the three are one corner.
    const auto time = [&](size_t i) { return samples[i].session_time; };
    for (size_t k = 1; k + 1 < joined.size();) {
        const auto& before = joined[k - 1];
        const auto& catchRun = joined[k];
        const auto& after = joined[k + 1];
        if (before.side == after.side && catchRun.side != before.side &&
            time(catchRun.last) - time(catchRun.first) < kCorrectionMaxS &&
            time(catchRun.first) - time(before.last) < kCornerJoinS &&
            time(after.first) - time(catchRun.last) < kCornerJoinS) {
            joined[k - 1].last = after.last;
            joined.erase(joined.begin() + static_cast<std::ptrdiff_t>(k),
                         joined.begin() + static_cast<std::ptrdiff_t>(k + 2));
        } else {
            ++k;
        }
    }
    joined.erase(std::remove_if(joined.begin(), joined.end(), [&](const SteeringCorner& corner) {
        return samples[corner.last].session_time - samples[corner.first].session_time < kCornerMinS;
    }), joined.end());
    return joined;
}

// The corners whose apex falls between two session times, from the steering:
// each apex where the driver has done half the corner's steering, summed over
// time, and the slowest speed through each. The wheel's furthest point is not
// used: through a corner taken on a sawing wheel (São Paulo's second Senna S)
// it lands on whichever spike was largest. Over 8 recorded circuits, against
// the tightest point of each corner's path averaged over every lap, the half
// point is a median 10 m off (the furthest point 13 m), and a corner's apex
// moves 6 m from lap to lap (13 m). Without steering, speed dips stand in
// (findApexes).
//
// With the recording's track corners, those stand in for the steering: each
// corner the lap is in at any point between the two times, at the track's
// apex, with the lap's slowest speed from the corner's turn-in to its exit.
std::vector<CornerApex> findApexes(const detail::ProgressMap& progress,
                                   const std::vector<InputSample>& samples, float from, float to);
float timeAtDistance(const detail::ProgressMap& progress, float distance);
std::vector<CornerApex> findCorners(const detail::ProgressMap& progress,
                                    const std::vector<InputSample>& samples, float from, float to,
                                    const TrackCorners* track) {
    if (track && !track->corners.empty() && !progress.points.empty()) {
        const float lapStart = progress.points.front().session_time;
        const float lapEnd = progress.points.back().session_time;
        std::vector<CornerApex> out;
        for (const auto& corner : track->corners) {
            const float at = timeAtDistance(progress, corner.apexM);
            if (!std::isfinite(at)) continue;
            // A corner across the line is cut at it, on the apex's side.
            const bool wraps = corner.fromM > corner.toM;
            float t0 = wraps && corner.apexM < corner.fromM ? lapStart : timeAtDistance(progress, corner.fromM);
            float t1 = wraps && corner.apexM >= corner.fromM ? lapEnd : timeAtDistance(progress, corner.toM);
            if (!std::isfinite(t0)) t0 = lapStart;
            if (!std::isfinite(t1)) t1 = lapEnd;
            // Any of the corner between the two times counts: a lap can be
            // flat out again before a long corner's apex.
            if (t1 < from || t0 > to) continue;
            std::optional<float> slowest;
            float nearest = std::numeric_limits<float>::infinity();
            float atApex = 0.0f;
            for (const auto& sample : samples) {
                if (sample.session_time >= t0 && sample.session_time <= t1)
                    slowest = std::min(slowest.value_or(sample.speed_kph), sample.speed_kph);
                if (std::abs(sample.session_time - at) < nearest) {
                    nearest = std::abs(sample.session_time - at);
                    atApex = sample.speed_kph;
                }
            }
            out.push_back({corner.apexM, slowest.value_or(atApex), corner.side});
            out.back().fromM = corner.fromM;
            out.back().toM = corner.toM;
        }
        return out;
    }
    const auto corners = steeringCorners(samples);
    if (corners.empty()) return findApexes(progress, samples, from, to);
    std::vector<CornerApex> out;
    for (const auto& corner : corners) {
        // Steering into the turn, held until the next sample.
        const auto turning = [&](size_t i) {
            const float dt = samples[i + 1].session_time - samples[i].session_time;
            return std::max(0.0f, samples[i].steering * static_cast<float>(corner.side)) * std::max(0.0f, dt);
        };
        double total = 0.0;
        float slowest = samples[corner.first].speed_kph;
        for (size_t i = corner.first; i <= corner.last; ++i) {
            if (i < corner.last) total += turning(i);
            slowest = std::min(slowest, samples[i].speed_kph);
        }
        float at = samples[corner.first].session_time;
        double sum = 0.0;
        for (size_t i = corner.first; i < corner.last && total > 0.0; ++i) {
            const double step = turning(i);
            if (sum + step >= total / 2.0) {
                const double ratio = step > 0.0 ? (total / 2.0 - sum) / step : 0.0;
                at = samples[i].session_time + static_cast<float>(ratio) *
                     (samples[i + 1].session_time - samples[i].session_time);
                break;
            }
            sum += step;
        }
        if (at < from || at > to) continue;
        const float m = distanceAtTime(progress, at);
        if (!(m >= 0.0f)) continue;
        out.push_back({m, slowest, corner.side});
        // The corner runs for as long as the wheel is turned.
        const float fromM = distanceAtTime(progress, samples[corner.first].session_time);
        const float toM = distanceAtTime(progress, samples[corner.last].session_time);
        if (fromM >= 0.0f && toM >= fromM) {
            out.back().fromM = fromM;
            out.back().toM = toM;
        }
    }
    return out;
}

// The corners between two session times: each dip in speed at least
// kApexProminenceKph deep on both sides, so lifts count as well as braked
// corners. A dip still open at the end counts.
std::vector<CornerApex> findApexes(const detail::ProgressMap& progress,
                                   const std::vector<InputSample>& samples, float from, float to) {
    std::vector<CornerApex> apexes;
    std::optional<float> peakKph;
    std::optional<size_t> low;
    const auto add = [&](size_t i) {
        const float m = distanceAtTime(progress, samples[i].session_time);
        if (m >= 0.0f) apexes.push_back({m, samples[i].speed_kph});
    };
    for (size_t i = 0; i < samples.size(); ++i) {
        if (samples[i].session_time < from) continue;
        if (samples[i].session_time > to) break;
        const float speed = samples[i].speed_kph;
        if (!peakKph) peakKph = speed;
        if (!low) {
            if (speed <= *peakKph - kApexProminenceKph) low = i;
            else peakKph = std::max(*peakKph, speed);
        } else if (speed < samples[*low].speed_kph) {
            low = i;
        } else if (speed >= samples[*low].speed_kph + kApexProminenceKph) {
            add(*low);
            low.reset();
            peakKph = speed;
        }
    }
    if (low) add(*low);
    return apexes;
}

// The zone's minimum speed is its slowest corner.
void setMinimumFromApexes(BrakingPoint& point) {
    const auto lowest = std::min_element(point.apexes.begin(), point.apexes.end(),
        [](const CornerApex& x, const CornerApex& y) { return x.speedKph < y.speedKph; });
    if (lowest == point.apexes.end()) return;
    point.minSpeedKph = lowest->speedKph;
    point.minSpeedM = std::max(lowest->distanceM, point.distanceM);
}

PedalState pedalState(const InputSample& sample) {
    if (sample.brake > kPedalPressed) return PedalState::Braking;
    if (sample.throttle >= kFullThrottle) return PedalState::FullThrottle;
    if (sample.throttle > kPedalPressed) return PedalState::PartialThrottle;
    return PedalState::Coasting;
}

// How long each lap took over [start, end], per equal mini-sector of `sectorM`
// from the line, over the distance both laps cover. NaN where either is unknown.
struct SectorTimes {
    float length{};
    std::vector<double> current;
    std::vector<double> comparison;
};
SectorTimes sectorTimes(const AnalysisLapProgress& currentLap,
                        const AnalysisLapProgress& comparisonLap, float sectorM) {
    SectorTimes out;
    const auto a = detail::buildProgressMap(currentLap);
    const auto b = detail::buildProgressMap(comparisonLap);
    if (a.points.empty() || b.points.empty()) return out;
    const float length = currentLap.trackLengthM > 0.0f ? currentLap.trackLengthM
        : comparisonLap.trackLengthM > 0.0f ? comparisonLap.trackLengthM
        : std::max(a.maxDistance, b.maxDistance);
    if (!(length > 0.0f)) return out;
    out.length = length;

    // Lap rows start a few metres past the line and stop short of the full
    // length, so the first and last mini-sectors compare only what both laps
    // cover.
    const float covered0 = std::max(a.points.front().lap_distance_m, b.points.front().lap_distance_m);
    const float covered1 = std::min(a.maxDistance, b.maxDistance);
    const int count = std::max(1, static_cast<int>(std::lround(length / sectorM)));
    const float step = length / static_cast<float>(count);
    const double nan = std::numeric_limits<double>::quiet_NaN();
    out.current.assign(static_cast<size_t>(count), nan);
    out.comparison.assign(static_cast<size_t>(count), nan);
    for (int k = 0; k < count; ++k) {
        const float start = std::max(covered0, k * step);
        const float end = std::min(covered1, (k + 1) * step);
        if (!(end > start)) continue;
        out.current[static_cast<size_t>(k)] =
            detail::interpolateElapsed(a, end) - detail::interpolateElapsed(a, start);
        out.comparison[static_cast<size_t>(k)] =
            detail::interpolateElapsed(b, end) - detail::interpolateElapsed(b, start);
    }
    return out;
}

struct LapRead {
    AnalysisLapProgress progress;
    std::vector<InputSample> inputs;
    std::vector<HarvestSample> harvest;
    std::vector<ErsSample> ers;
    std::vector<TrackStatusChange> trackStatus;
    ErsPeaks peaks;
    // The recording's track corners, when its sweep found them.
    std::optional<TrackCorners> corners;
    // For comparing braking zones: the lap's world positions, and when this
    // driver was called for track limits anywhere in the recording.
    std::vector<PlayerPositionPoint> positions;
    std::vector<float> trackLimits;
    bool ok{};
};

// The recording's track corners. A V6 recording's sweep started at load has
// usually finished; it is waited for outside the recording's lock. Without
// one (formats that hold only the playback car) that car's laps are read
// once, a lap per lock, and the corners kept on the reader.
std::optional<TrackCorners> readCorners(const AnalysisMapLap& request) {
    std::shared_ptr<TrackCornerSweep> sweep;
    std::optional<TrackCorners> cached;
    std::vector<int> laps;
    uint64_t serial = 0;
    if (!request.recording([&](const TnrdReader& reader) {
            sweep = reader.trackCornerSweep();
            if (sweep) return;
            TrackCorners corners;
            if (reader.cachedTrackCorners(corners)) cached = std::move(corners);
            else {
                laps = reader.analysisLapNumbers(-1);
                serial = reader.loadSerial();
            }
        })) return std::nullopt;
    TrackCorners corners;
    if (sweep) return sweep->waitFor(corners) ? std::optional<TrackCorners>(std::move(corners)) : std::nullopt;
    if (cached) return cached->corners.empty() ? std::nullopt : cached;

    std::optional<TrackPathAverage> average;
    for (const int lapNum : laps) {
        AnalysisLapProgress lap;
        std::vector<PlayerPositionPoint> positions;
        bool read = false;
        if (!request.recording([&](const TnrdReader& reader) {
                read = reader.getAnalysisLapProgress(lapNum, lap, -1) && lap.lapTimeMs > 0 &&
                       reader.getAnalysisPositionSamples(lapNum, positions, -1);
            })) return std::nullopt;
        if (!read || !(lap.trackLengthM > 0.0f)) continue;
        if (!average) average.emplace(lap.trackLengthM);
        std::vector<float> time;
        std::vector<double> x, z;
        for (const auto& point : positions) {
            time.push_back(point.session_time);
            x.push_back(point.x);
            z.push_back(point.z);
        }
        std::vector<std::pair<float, double>> lapDistance;
        for (const auto& point : lap.points) lapDistance.emplace_back(point.session_time, point.lap_distance_m);
        (void)average->addLap(time, x, z, lapDistance);
    }
    if (average) corners = average->corners();
    (void)request.recording([&](const TnrdReader& reader) { reader.cacheTrackCorners(serial, corners); });
    if (corners.corners.empty()) return std::nullopt;
    return corners;
}

// Reads one lap, and for ERS Usage the car's whole-recording peaks: from the
// reader's cache, or by reading each of the car's laps in turn.
LapRead readLap(const AnalysisMapLap& request, bool wantErs, bool wantInputs, bool wantHarvest,
                bool wantPeaks = true) {
    LapRead out;
    std::vector<int> carLaps;
    bool havePeaks = false;
    uint64_t serial = 0;
    int car = -1;
    std::shared_ptr<ErsPeakSweep> sweep;
    const bool open = request.recording([&](const TnrdReader& reader) {
        out.ok = reader.getAnalysisLapProgress(request.lapNum, out.progress, request.driverIndex);
        if (out.ok && wantInputs)
            (void)reader.getAnalysisInputSamples(request.lapNum, out.inputs, request.driverIndex);
        if (!out.ok || !wantErs) return;
        (void)reader.getAnalysisErsSamples(request.lapNum, out.ers, request.driverIndex);
        if (wantHarvest)
            (void)reader.getAnalysisHarvestSamples(request.lapNum, out.harvest, request.driverIndex);
        out.trackStatus = reader.aeroTrackStatusTimeline();
        if (!wantPeaks) return;
        havePeaks = reader.cachedErsPeaks(request.driverIndex, out.peaks);
        if (!havePeaks) {
            carLaps = reader.analysisLapNumbers(request.driverIndex);
            serial = reader.loadSerial();
            car = reader.analysisCarIndex(request.driverIndex);
            sweep = reader.ersPeakSweep();
        }
    });
    if (!open || !out.ok || !wantErs || !wantPeaks || havePeaks) {
        out.ok = open && out.ok;
        return out;
    }
    // The sweep started at load has usually finished; wait for it outside the
    // recording's lock rather than scanning the car again.
    if (sweep && sweep->waitFor(car, out.peaks)) {
        (void)request.recording([&](const TnrdReader& reader) {
            reader.cacheErsPeaks(request.driverIndex, serial, out.peaks);
        });
        return out;
    }

    ErsPeaks peaks;
    for (const int lapNum : carLaps) {
        AnalysisLapProgress bounds;
        std::vector<ErsSample> samples;
        std::vector<HarvestSample> harvest;
        bool read = false;
        if (!request.recording([&](const TnrdReader& reader) {
                read = reader.getAnalysisLapProgress(lapNum, bounds, request.driverIndex) &&
                    reader.getAnalysisErsSamples(lapNum, samples, request.driverIndex);
                if (read) (void)reader.getAnalysisHarvestSamples(lapNum, harvest, request.driverIndex);
            })) return out;
        if (read) {
            accumulateErsPeaks(peaks, samples, bounds.startSessionTime,
                               bounds.endSessionTime, out.trackStatus);
            accumulateHarvestPeak(peaks, harvest, bounds.startSessionTime, bounds.endSessionTime);
        }
    }
    out.peaks = peaks;
    (void)request.recording([&](const TnrdReader& reader) {
        reader.cacheErsPeaks(request.driverIndex, serial, peaks);
    });
    return out;
}

// Session time at a lap distance along the cleaned progress curve; NaN
// outside it.
float timeAtDistance(const detail::ProgressMap& progress, float distance) {
    const auto& points = progress.points;
    if (points.size() < 2 || distance < points.front().lap_distance_m ||
        distance > progress.maxDistance) return std::numeric_limits<float>::quiet_NaN();
    const auto after = std::lower_bound(
        std::next(points.begin()), points.end(), distance,
        [](const LapProgressPoint& point, float value) { return point.lap_distance_m < value; });
    if (after == points.end()) return points.back().session_time;
    const auto& before = *std::prev(after);
    const float span = after->lap_distance_m - before.lap_distance_m;
    const float ratio = span > 0.0f ? (distance - before.lap_distance_m) / span : 1.0f;
    return before.session_time + (after->session_time - before.session_time) * ratio;
}

// One lap's ERS over a braking zone: kJ harvested across it and MJ in the
// store entering it, -1 each when unknown.
std::pair<float, float> zoneEnergy(const LapRead& lap, float fromM, float toM) {
    if (lap.ers.empty() || isRestricted(lap.ers)) return {-1.0f, -1.0f};
    const auto progress = detail::buildProgressMap(lap.progress);
    const float t0 = timeAtDistance(progress, fromM);
    const float t1 = timeAtDistance(progress, toM);
    if (!std::isfinite(t0) || !std::isfinite(t1) || !(t1 > t0)) return {-1.0f, -1.0f};
    double gained = 0.0;
    for (size_t i = 0; i + 1 < lap.harvest.size(); ++i) {
        const auto& a = lap.harvest[i];
        const auto& b = lap.harvest[i + 1];
        if (a.session_time < t0 || b.session_time > t1) continue;
        gained += std::max(0.0, b.harvested_j - a.harvested_j);   // a drop is the line reset
    }
    // The store's latest reading at or before the zone starts.
    float store = -1.0f;
    for (const auto& sample : lap.ers) {
        if (sample.session_time > t0) break;
        store = static_cast<float>(sample.ers_j / 1e6);
    }
    if (store < 0.0f) store = static_cast<float>(lap.ers.front().ers_j / 1e6);
    return {lap.harvest.empty() ? -1.0f : static_cast<float>(gained / 1000.0), store};
}

struct WorldPoint { double x{}; double z{}; };

// A lap's world position at a session time; nullopt outside its samples.
std::optional<WorldPoint> positionAtTime(const std::vector<PlayerPositionPoint>& points, float time) {
    if (points.size() < 2 || !std::isfinite(time) || time < points.front().session_time ||
        time > points.back().session_time) return std::nullopt;
    const auto after = std::lower_bound(
        std::next(points.begin()), points.end(), time,
        [](const PlayerPositionPoint& point, float value) { return point.session_time < value; });
    if (after == points.end()) return WorldPoint{points.back().x, points.back().z};
    const auto& before = *std::prev(after);
    const float span = after->session_time - before.session_time;
    const double ratio = span > 0.0f ? (time - before.session_time) / span : 1.0;
    return WorldPoint{before.x + (after->x - before.x) * ratio, before.z + (after->z - before.z) * ratio};
}

// Session time at a lap distance, held at the lap's first and last recorded
// distances past them, so a stretch reaching over the line stops at it.
float timeAtDistanceClamped(const detail::ProgressMap& progress, float distance) {
    if (progress.points.size() < 2) return std::numeric_limits<float>::quiet_NaN();
    const float first = progress.points.front().lap_distance_m;
    return timeAtDistance(progress, std::max(first, std::min(distance, progress.maxDistance)));
}

// The unit vector towards the inside of the bend where a lap's path is at a
// lap distance. `side` is the way the corner turns (-1 left, 1 right): the
// inside is square to the path on that side, so neighbouring corners cannot
// tip it over. Positive curvature is turning right (TrackPathAverage::corners),
// which in (x, z) is the path's direction turned a quarter towards +z. With no
// side, it is where the path turns towards across kLineChordM either side.
// Nullopt where the positions do not cover it or, with no side, it barely turns.
std::optional<WorldPoint> insideAt(const LapRead& lap, const detail::ProgressMap& progress,
                                   float distance, int side) {
    if (lap.positions.size() < 2) return std::nullopt;
    // Held at the ends of the lap's positions: a point next to the line still
    // has a direction.
    const auto near = [&](float d) -> std::optional<WorldPoint> {
        const float time = timeAtDistanceClamped(progress, d);
        if (!std::isfinite(time)) return std::nullopt;
        return positionAtTime(lap.positions, std::max(lap.positions.front().session_time,
                                                      std::min(time, lap.positions.back().session_time)));
    };
    const auto heading = [](const WorldPoint& from, const WorldPoint& to) -> std::optional<WorldPoint> {
        const double dx = to.x - from.x, dz = to.z - from.z, length = std::hypot(dx, dz);
        if (!(length > 0.0)) return std::nullopt;
        return WorldPoint{dx / length, dz / length};
    };
    if (side != 0) {
        const auto before = near(distance - kLineTangentM);
        const auto after = near(distance + kLineTangentM);
        if (!before || !after) return std::nullopt;
        const auto along = heading(*before, *after);
        if (!along) return std::nullopt;
        return WorldPoint{-along->z * side, along->x * side};
    }
    const auto before = near(distance - kLineChordM);
    const auto at = near(distance);
    const auto after = near(distance + kLineChordM);
    if (!before || !at || !after) return std::nullopt;
    const auto in = heading(*before, *at);
    const auto out = heading(*at, *after);
    if (!in || !out) return std::nullopt;
    const WorldPoint inside{out->x - in->x, out->z - in->z};
    const double turn = std::hypot(inside.x, inside.z);
    if (turn < kLineMinTurn) return std::nullopt;
    return WorldPoint{inside.x / turn, inside.z / turn};
}

// How much further inside the bend lap `a` runs than lap `b` at one point of a
// corner, in metres (negative when wider): lap `a` at `distanceA`, against the
// nearest point of lap `b`'s path around `distanceB`, because equal lap
// distances are not level with each other on different lines. Nullopt where
// the positions do not cover the point or the inside cannot be told.
std::optional<float> lineGap(const LapRead& a, const detail::ProgressMap& progressA, float distanceA, int side,
                             const LapRead& b, const detail::ProgressMap& progressB, float distanceB) {
    const auto apex = positionAtTime(a.positions, timeAtDistance(progressA, distanceA));
    if (!apex) return std::nullopt;
    const auto found = insideAt(a, progressA, distanceA, side);
    if (!found) return std::nullopt;
    const WorldPoint inside = *found;

    const float t0 = timeAtDistanceClamped(progressB, distanceB - kLineSearchM);
    const float t1 = timeAtDistanceClamped(progressB, distanceB + kLineSearchM);
    if (!std::isfinite(t0) || !std::isfinite(t1)) return std::nullopt;
    double nearest = kLineMaxGapM;
    std::optional<WorldPoint> closest;
    for (size_t i = 0; i + 1 < b.positions.size(); ++i) {
        const auto& p = b.positions[i];
        const auto& q = b.positions[i + 1];
        if (q.session_time < t0 || p.session_time > t1) continue;
        const double dx = q.x - p.x, dz = q.z - p.z, lengthSq = dx * dx + dz * dz;
        const double t = lengthSq > 0.0
            ? std::clamp(((apex->x - p.x) * dx + (apex->z - p.z) * dz) / lengthSq, 0.0, 1.0) : 0.0;
        const WorldPoint point{p.x + dx * t, p.z + dz * t};
        const double distance = std::hypot(apex->x - point.x, apex->z - point.z);
        if (distance <= nearest) { nearest = distance; closest = point; }
    }
    if (!closest) return std::nullopt;
    return static_cast<float>((apex->x - closest->x) * inside.x + (apex->z - closest->z) * inside.z);
}

// The tightest radius a lap's own path turns at through a corner, in metres:
// its positions taken metre by metre of the distance it travelled, with the
// heading and curvature windows the track corners use, so two laps read alike.
// Turning the corner's way only, when its side is known. 0 when the corner's
// extent is unknown, the positions do not cover it, the car jumped, or the
// path never turns tighter than kTrackCornerEndRadiusM there.
float cornerRadius(const LapRead& lap, const detail::ProgressMap& progress, const CornerApex& apex) {
    if (apex.fromM < 0.0f || apex.toM < 0.0f || progress.points.size() < 2) return 0.0f;
    // A corner across the line is cut at it, on the apex's side.
    const float fromM = apex.fromM <= apex.distanceM ? apex.fromM : progress.points.front().lap_distance_m;
    const float toM = apex.toM >= apex.distanceM ? apex.toM : progress.maxDistance;
    const int margin = kPathHeadingM + kPathCurvatureM;
    const float t0 = timeAtDistanceClamped(progress, fromM);
    const float t1 = timeAtDistanceClamped(progress, toM);
    const float w0 = timeAtDistanceClamped(progress, fromM - static_cast<float>(margin));
    const float w1 = timeAtDistanceClamped(progress, toM + static_cast<float>(margin));
    if (!std::isfinite(t0) || !std::isfinite(t1) || !std::isfinite(w0) || !std::isfinite(w1) || !(t1 > t0))
        return 0.0f;

    // The path around the corner, and how far along it each position is.
    std::vector<WorldPoint> raw;
    std::vector<double> travelled;
    double cornerFrom = -1.0, cornerTo = -1.0;
    for (const auto& p : lap.positions) {
        if (p.session_time < w0) continue;
        if (p.session_time > w1) break;
        double along = 0.0;
        if (!raw.empty()) {
            const double step = std::hypot(p.x - raw.back().x, p.z - raw.back().z);
            if (step > kPathMaxStepM) return 0.0f;
            along = travelled.back() + step;
        }
        if (cornerFrom < 0.0 && p.session_time >= t0) cornerFrom = along;
        if (p.session_time <= t1) cornerTo = along;
        raw.push_back({p.x, p.z});
        travelled.push_back(along);
    }
    if (raw.size() < 2 || cornerFrom < 0.0 || cornerTo < cornerFrom) return 0.0f;
    const int count = static_cast<int>(travelled.back()) + 1;
    if (count <= 2 * margin) return 0.0f;
    std::vector<WorldPoint> path(static_cast<size_t>(count));
    size_t j = 0;
    for (int k = 0; k < count; ++k) {
        while (j + 2 < raw.size() && travelled[j + 1] < k) ++j;
        const double span = travelled[j + 1] - travelled[j];
        const double ratio = span > 0.0 ? std::clamp((k - travelled[j]) / span, 0.0, 1.0) : 0.0;
        path[static_cast<size_t>(k)] = {raw[j].x + (raw[j + 1].x - raw[j].x) * ratio,
                                        raw[j].z + (raw[j + 1].z - raw[j].z) * ratio};
    }
    const auto heading = [&](int k) {
        const auto& before = path[static_cast<size_t>(k - kPathHeadingM)];
        const auto& after = path[static_cast<size_t>(k + kPathHeadingM)];
        return std::atan2(after.z - before.z, after.x - before.x);
    };
    // Where both windows fit inside the path read.
    const int first = std::max(margin, static_cast<int>(std::lround(cornerFrom)));
    const int last = std::min(count - 1 - margin, static_cast<int>(std::lround(cornerTo)));
    double peak = 0.0;
    for (int k = first; k <= last; ++k) {
        const double turn = std::remainder(heading(k + kPathCurvatureM) - heading(k - kPathCurvatureM),
                                           2.0 * std::numbers::pi);
        const double curvature = turn / (2.0 * kPathCurvatureM);
        peak = std::max(peak, apex.side != 0 ? curvature * apex.side : std::abs(curvature));
    }
    return peak > 1.0 / kTrackCornerEndRadiusM ? static_cast<float>(1.0 / peak) : 0.0f;
}

// The corners of a zone a lap was called for track limits at, as indices into
// its apexes: the last corner reached before each call. A call counts from the
// zone's start to kTrackLimitLagS after its end; one falling in that tail of
// the previous zone stays with that zone. `previousToM` is -1 for the first zone.
std::vector<int> trackLimitCorners(const LapRead& lap, const detail::ProgressMap& progress,
                                   const BrakingPoint& point, const BrakingPointPair& pair, float previousToM) {
    std::vector<int> corners;
    float from = timeAtDistance(progress, pair.fromM);
    const float end = timeAtDistance(progress, pair.toM);
    if (!std::isfinite(from) || !std::isfinite(end) || progress.points.empty()) return corners;
    if (previousToM >= 0.0f) {
        const float previous = timeAtDistance(progress, previousToM);
        if (std::isfinite(previous)) from = std::max(from, previous + kTrackLimitLagS);
    }
    for (const float time : lap.trackLimits) {
        if (time < from || time > end + kTrackLimitLagS) continue;
        const auto reached = std::lower_bound(
            progress.points.begin(), progress.points.end(), time,
            [](const LapProgressPoint& sample, float value) { return sample.session_time < value; });
        const float distance = reached == progress.points.end()
            ? progress.points.back().lap_distance_m : reached->lap_distance_m;
        int corner = 0;
        for (size_t i = 0; i < point.apexes.size(); ++i)
            if (point.apexes[i].distanceM <= distance) corner = static_cast<int>(i);
        corners.push_back(corner);
    }
    return corners;
}

// Each matched braking zone's ERS figures, line comparison and observations.
void describeBrakingZones(BrakingPointsMap& braking, const LapRead& current, const LapRead& comparison) {
    const auto progressA = detail::buildProgressMap(current.progress);
    const auto progressB = detail::buildProgressMap(comparison.progress);
    for (size_t zone = 0; zone < braking.pairs.size(); ++zone) {
        auto& pair = braking.pairs[zone];
        // Harvest is under braking: over the braking half when the zone splits.
        const float harvestTo = pair.splitM >= 0.0f ? pair.splitM : pair.toM;
        std::tie(pair.currentHarvestKj, pair.currentStoreMj) = zoneEnergy(current, pair.fromM, harvestTo);
        std::tie(pair.comparisonHarvestKj, pair.comparisonStoreMj) = zoneEnergy(comparison, pair.fromM, harvestTo);
        auto& a = braking.current[static_cast<size_t>(pair.current)];
        auto& b = braking.comparison[static_cast<size_t>(pair.comparison)];
        auto& out = pair.observations;
        const bool timed = pair.currentTimeS >= 0.0f && pair.comparisonTimeS >= 0.0f;
        const int quicker = !timed || pair.currentTimeS == pair.comparisonTimeS ? -1
            : pair.currentTimeS < pair.comparisonTimeS ? 0 : 1;
        const float gainS = timed ? std::abs(pair.comparisonTimeS - pair.currentTimeS) : 0.0f;
        const auto& quick = quicker == 0 ? a : b;
        const auto& slow = quicker == 0 ? b : a;

        // Each lap's own radius through each corner.
        for (auto& apex : a.apexes) apex.radiusM = cornerRadius(current, progressA, apex);
        for (auto& apex : b.apexes) apex.radiusM = cornerRadius(comparison, progressB, apex);

        // Each corner's lines, and the corner where they differ most.
        float widestGap = 0.0f;   // > 0: the current lap is further inside at the apex
        int widestA = -1, widestB = -1;
        for (size_t i = 0; i < a.apexes.size(); ++i) {
            auto& apex = a.apexes[i];
            int match = -1;
            float nearest = kLineCornerMatchM;
            for (size_t j = 0; j < b.apexes.size(); ++j) {
                const float apart = std::abs(b.apexes[j].distanceM - apex.distanceM);
                if (apart <= nearest) { nearest = apart; match = static_cast<int>(j); }
            }
            if (match < 0) continue;
            auto& other = b.apexes[static_cast<size_t>(match)];
            const auto gap = lineGap(current, progressA, apex.distanceM, apex.side,
                                     comparison, progressB, other.distanceM);
            if (!gap) continue;
            const auto level = [](float metres) { return std::abs(metres) < kLineLevelM ? 0.0f : metres; };
            const float shown = level(*gap);
            apex.hasLine = other.hasLine = true;
            apex.lineGapM = shown;
            other.lineGapM = -shown;
            // The same at turn-in and at the exit, where the corner has both
            // on this lap: with the apex they give the line's shape.
            if (apex.fromM >= 0.0f && apex.fromM <= apex.distanceM && apex.toM >= apex.distanceM) {
                const auto entry = lineGap(current, progressA, apex.fromM, apex.side,
                                           comparison, progressB, apex.fromM);
                const auto exit = lineGap(current, progressA, apex.toM, apex.side,
                                          comparison, progressB, apex.toM);
                if (entry && exit) {
                    apex.hasLineEnds = other.hasLineEnds = true;
                    apex.lineEntryM = level(*entry);
                    apex.lineExitM = level(*exit);
                    other.lineEntryM = -apex.lineEntryM;
                    other.lineExitM = -apex.lineExitM;
                }
            }
            if (std::abs(shown) > std::abs(widestGap)) {
                widestGap = shown;
                widestA = static_cast<int>(i);
                widestB = match;
            }
        }

        // What the lap further inside at that apex did, and whether it helped
        // or hurt. A track-limits call at that corner decides it. Otherwise
        // the line's shape does: inside at the apex from a wider turn-in and
        // exit, or on a larger radius of its own, is the wider arc; anything
        // else is the tighter line, and one that lost time is told why. Time
        // is taken over the corner itself when the zone has more than one, so
        // another corner's time does not count for this one's line.
        const float previousToM = zone > 0 ? braking.pairs[zone - 1].toM : -1.0f;
        const auto limitsA = trackLimitCorners(current, progressA, a, pair, previousToM);
        const auto limitsB = trackLimitCorners(comparison, progressB, b, pair, previousToM);
        bool limitsSaid[2] = {false, false};
        if (std::abs(widestGap) >= kObserveLineM) {
            const int lap = widestGap > 0.0f ? 0 : 1;
            const float metres = std::abs(widestGap);
            const auto& tight = lap == 0 ? a : b;
            const auto& rival = lap == 0 ? b : a;
            const auto& limits = lap == 0 ? limitsA : limitsB;
            const int corner = lap == 0 ? widestA : widestB;
            const int rivalCorner = lap == 0 ? widestB : widestA;
            const auto& mine = tight.apexes[static_cast<size_t>(corner)];
            const auto& theirs = rival.apexes[static_cast<size_t>(rivalCorner)];
            const bool wideEnds = mine.hasLineEnds && mine.lineEntryM <= -kObserveLineM &&
                                  mine.lineExitM <= -kObserveLineM;
            const bool largerRadius = mine.radiusM > 0.0f && theirs.radiusM > 0.0f &&
                                      mine.radiusM >= theirs.radiusM * (1.0f + kObserveRadiusShare);
            // Seconds the lap gained on the other: over the corner, from the
            // earlier turn-in to the later exit, or over the zone when this is
            // its only corner or the corner's extent is unknown.
            bool lineTimed = timed;
            float gained = lap == 0 ? pair.comparisonTimeS - pair.currentTimeS
                                    : pair.currentTimeS - pair.comparisonTimeS;
            const auto whole = [](const CornerApex& apex) {
                return apex.fromM >= 0.0f && apex.fromM <= apex.distanceM && apex.toM >= apex.distanceM;
            };
            if ((a.apexes.size() > 1 || b.apexes.size() > 1) && whole(mine) && whole(theirs)) {
                const float fromM = std::min(mine.fromM, theirs.fromM);
                const float toM = std::max(mine.toM, theirs.toM);
                const auto over = [&](const detail::ProgressMap& progress) {
                    return detail::interpolateElapsed(progress, toM) - detail::interpolateElapsed(progress, fromM);
                };
                const double tightS = over(lap == 0 ? progressA : progressB);
                const double rivalS = over(lap == 0 ? progressB : progressA);
                if (std::isfinite(tightS) && std::isfinite(rivalS) && tightS > 0.0 && rivalS > 0.0) {
                    lineTimed = true;
                    gained = static_cast<float>(rivalS - tightS);
                }
            }
            if (std::find(limits.begin(), limits.end(), corner) != limits.end()) {
                out.push_back({"tighter_track_limits", lap, metres, 0.0f});
                limitsSaid[lap] = true;
            } else if (wideEnds || largerRadius) {
                out.push_back({"straighter_line", lap, metres,
                               lineTimed && std::abs(gained) >= kObserveLevelS ? gained : 0.0f});
            } else if (lineTimed) {
                const float apexLoss = theirs.speedKph - mine.speedKph;
                const auto laterBy = [](float tightM, float rivalM) {
                    return tightM >= 0.0f && rivalM >= 0.0f ? tightM - rivalM : 0.0f;
                };
                const float exitLoss = std::max(laterBy(tight.throttleOnM, rival.throttleOnM),
                                                laterBy(tight.fullThrottleM, rival.fullThrottleM));
                if (std::abs(gained) < kObserveLevelS) out.push_back({"tighter_no_gain", lap, metres, 0.0f});
                else if (gained > 0.0f) out.push_back({"tighter_helped", lap, metres, gained});
                else if (apexLoss >= kObserveApexKph) out.push_back({"tighter_lost_speed", lap, metres, apexLoss});
                else if (tight.wheelspin && !rival.wheelspin)
                    out.push_back({"tighter_wheelspin", lap, metres, tight.wheelspinPct});
                else if (exitLoss >= kObserveExitM) out.push_back({"tighter_slower_exit", lap, metres, exitLoss});
                else out.push_back({"tighter_slower", lap, metres, -gained});
            }
        }
        for (int lap = 0; lap < 2; ++lap)
            if (!limitsSaid[lap] && !(lap == 0 ? limitsA : limitsB).empty())
                out.push_back({"track_limits", lap, 0.0f, 0.0f});

        // One lap takes it on a lift where the other brakes.
        if (a.liftOnly != b.liftOnly) out.push_back({"lift_only", a.liftOnly ? 0 : 1, 0.0f, 0.0f});
        // Quicker despite coasting longer: the lift cost nothing.
        if (quicker >= 0 && quick.coastS - slow.coastS >= kObserveCoastS)
            out.push_back({"quicker_despite_coast", quicker, gainS, quick.coastS - slow.coastS});
        // More speed through the apex.
        if (std::abs(a.minSpeedKph - b.minSpeedKph) >= kObserveApexKph) {
            const int lap = a.minSpeedKph > b.minSpeedKph ? 0 : 1;
            out.push_back({"apex_speed", lap, lap == 0 ? a.minSpeedKph : b.minSpeedKph,
                           lap == 0 ? b.minSpeedKph : a.minSpeedKph});
        }
        // Braking later did not pay.
        if (quicker >= 0 && pair.sameBrakingPoint && std::abs(pair.gapM) >= kObserveLaterM) {
            const int later = pair.gapM > 0.0f ? 0 : 1;
            if (later != quicker) out.push_back({"later_but_slower", later, std::abs(pair.gapM), gainS});
        }
        // Corner exit: wheelspin, who is back on the throttle earlier, and who
        // is flat out sooner. Getting on the throttle earlier only counts
        // without wheelspin; spinning is called out instead.
        for (int lap = 0; lap < 2; ++lap) {
            const auto& point = lap == 0 ? a : b;
            const auto& rival = lap == 0 ? b : a;
            if (!point.wheelspin) continue;
            const bool earlier = point.throttleOnM >= 0.0f && rival.throttleOnM >= 0.0f &&
                rival.throttleOnM - point.throttleOnM >= kObserveExitM;
            if (earlier) out.push_back({"earlier_throttle_spin", lap, rival.throttleOnM - point.throttleOnM, point.wheelspinPct});
            else out.push_back({"wheelspin", lap, point.wheelspinPct, point.wheelspinS});
        }
        if (a.throttleOnM >= 0.0f && b.throttleOnM >= 0.0f) {
            const float diff = b.throttleOnM - a.throttleOnM;   // > 0: current on earlier
            const int lap = diff > 0.0f ? 0 : 1;
            const auto& point = lap == 0 ? a : b;
            if (std::abs(diff) >= kObserveExitM && !point.wheelspin)
                out.push_back({"earlier_throttle", lap, std::abs(diff), 0.0f});
        }
        if (a.fullThrottleM >= 0.0f && b.fullThrottleM >= 0.0f && !a.wheelspin && !b.wheelspin) {
            const float diff = b.fullThrottleM - a.fullThrottleM;
            if (std::abs(diff) >= kObserveExitM)
                out.push_back({"faster_to_full", diff > 0.0f ? 0 : 1, std::abs(diff), 0.0f});
        }
        // Harvested noticeably more.
        if (pair.currentHarvestKj >= 0.0f && pair.comparisonHarvestKj >= 0.0f) {
            const float diff = pair.currentHarvestKj - pair.comparisonHarvestKj;
            const float larger = std::max(pair.currentHarvestKj, pair.comparisonHarvestKj);
            if (std::abs(diff) >= kObserveHarvestKj && std::abs(diff) >= kObserveHarvestShare * larger) {
                const int lap = diff > 0.0f ? 0 : 1;
                out.push_back({"more_harvest", lap, lap == 0 ? pair.currentHarvestKj : pair.comparisonHarvestKj,
                               lap == 0 ? pair.comparisonHarvestKj : pair.currentHarvestKj});
            }
        }
        // A near-empty battery entering the zone.
        if (pair.currentStoreMj >= 0.0f && pair.currentStoreMj < kLowBatteryMj)
            out.push_back({"low_battery", 0, pair.currentStoreMj, 0.0f});
        if (pair.comparisonStoreMj >= 0.0f && pair.comparisonStoreMj < kLowBatteryMj)
            out.push_back({"low_battery", 1, pair.comparisonStoreMj, 0.0f});
        // At most kMaxBrakingObservations for each half: braking, and the
        // corner exit.
        const auto exitKind = [](const BrakingObservation& o) {
            return o.kind == "wheelspin" || o.kind == "earlier_throttle_spin" ||
                   o.kind == "earlier_throttle" || o.kind == "faster_to_full" ||
                   o.kind == "tighter_wheelspin" || o.kind == "tighter_slower_exit";
        };
        size_t entries = 0, exits = 0;
        out.erase(std::remove_if(out.begin(), out.end(), [&](const BrakingObservation& o) {
            return ++(exitKind(o) ? exits : entries) > kMaxBrakingObservations;
        }), out.end());
    }
}

} // namespace

TrackPathAverage::TrackPathAverage(float trackLengthM)
    : trackLengthM_(trackLengthM > 0.0f ? trackLengthM : 0.0f) {
    const size_t count = static_cast<size_t>(std::ceil(trackLengthM_));
    sumX_.assign(count, 0.0);
    sumZ_.assign(count, 0.0);
    count_.assign(count, 0);
}

bool TrackPathAverage::addLap(const std::vector<float>& time, const std::vector<double>& x,
                              const std::vector<double>& z,
                              const std::vector<std::pair<float, double>>& lapDistance) {
    const size_t n = std::min({time.size(), x.size(), z.size()});
    if (count_.empty() || n < kPathMinSamples || lapDistance.size() < 2) return false;
    for (size_t i = 1; i < n; ++i)
        if (std::hypot(x[i] - x[i - 1], z[i] - z[i - 1]) > kPathMaxStepM) return false;
    // Each position's lap distance, from the timing samples either side.
    std::vector<double> distance(n, -1.0);
    size_t j = 0;
    for (size_t i = 0; i < n; ++i) {
        const float t = time[i];
        if (t < lapDistance.front().first || t > lapDistance.back().first) continue;
        while (j + 2 < lapDistance.size() && lapDistance[j + 1].first < t) ++j;
        const auto& [t0, d0] = lapDistance[j];
        const auto& [t1, d1] = lapDistance[j + 1];
        // Across the line the distance falls back; neither side is this lap's.
        if (d1 < d0) continue;
        const double ratio = t1 > t0 ? (t - t0) / (t1 - t0) : 0.0;
        distance[i] = d0 + (d1 - d0) * std::clamp(ratio, 0.0, 1.0);
    }
    // Each metre the lap passes, once, from the positions either side of it.
    const size_t count = count_.size();
    std::vector<bool> seen(count, false);
    bool any = false;
    for (size_t i = 1; i < n; ++i) {
        const double a = distance[i - 1], b = distance[i];
        if (a < 0.0 || b <= a || b - a > kPathMaxStepM) continue;
        for (double m = std::ceil(a); m <= b && m < static_cast<double>(count); m += 1.0) {
            const size_t k = static_cast<size_t>(m);
            if (seen[k]) continue;
            const double ratio = (m - a) / (b - a);
            sumX_[k] += x[i - 1] + (x[i] - x[i - 1]) * ratio;
            sumZ_[k] += z[i - 1] + (z[i] - z[i - 1]) * ratio;
            ++count_[k];
            seen[k] = any = true;
        }
    }
    if (any) ++laps_;
    return any;
}

void TrackPathAverage::merge(const TrackPathAverage& other) {
    if (other.count_.size() != count_.size()) return;
    for (size_t k = 0; k < count_.size(); ++k) {
        sumX_[k] += other.sumX_[k];
        sumZ_[k] += other.sumZ_[k];
        count_[k] += other.count_[k];
    }
    laps_ += other.laps_;
}

TrackCorners TrackPathAverage::corners() const {
    TrackCorners result;
    result.trackLengthM = trackLengthM_;
    const int count = static_cast<int>(count_.size());
    if (laps_ < kPathMinLaps || count < 4 * kPathCurvatureM) return result;
    const auto wrap = [count](int k) { return ((k % count) + count) % count; };

    // The averaged path, short gaps bridged.
    std::vector<double> px(static_cast<size_t>(count)), pz(static_cast<size_t>(count));
    int start = -1;
    for (int k = 0; k < count; ++k)
        if (count_[static_cast<size_t>(k)] > 0) { start = k; break; }
    if (start < 0) return result;
    for (int step = 0; step < count;) {
        const int k = wrap(start + step);
        int gap = 1;
        while (gap < count && count_[static_cast<size_t>(wrap(k + gap))] == 0) ++gap;
        if (gap - 1 > kPathMaxGapM) return result;
        const auto mean = [&](const std::vector<double>& sum, int at) {
            return sum[static_cast<size_t>(at)] / count_[static_cast<size_t>(at)];
        };
        const int next = wrap(k + gap);
        const double x0 = mean(sumX_, k), z0 = mean(sumZ_, k);
        const double x1 = mean(sumX_, next), z1 = mean(sumZ_, next);
        for (int g = 0; g < gap; ++g) {
            const double ratio = static_cast<double>(g) / gap;
            px[static_cast<size_t>(wrap(k + g))] = x0 + (x1 - x0) * ratio;
            pz[static_cast<size_t>(wrap(k + g))] = z0 + (z1 - z0) * ratio;
        }
        step += gap;
    }

    // Signed curvature: positive turning right, as the steering reads.
    std::vector<double> heading(static_cast<size_t>(count));
    for (int k = 0; k < count; ++k) {
        const size_t a = static_cast<size_t>(wrap(k - kPathHeadingM));
        const size_t b = static_cast<size_t>(wrap(k + kPathHeadingM));
        heading[static_cast<size_t>(k)] = std::atan2(pz[b] - pz[a], px[b] - px[a]);
    }
    std::vector<double> curvature(static_cast<size_t>(count));
    for (int k = 0; k < count; ++k) {
        double turn = heading[static_cast<size_t>(wrap(k + kPathCurvatureM))] -
                      heading[static_cast<size_t>(wrap(k - kPathCurvatureM))];
        turn = std::remainder(turn, 2.0 * std::numbers::pi);
        curvature[static_cast<size_t>(k)] = turn / (2.0 * kPathCurvatureM);
    }
    const auto sideAt = [&](int k, double radius) {
        const double c = curvature[static_cast<size_t>(wrap(k))];
        return std::abs(c) >= 1.0 / radius ? (c > 0.0 ? 1 : -1) : 0;
    };

    // Corners: runs tighter than kTrackCornerRadiusM, scanned from a point
    // that is not, so a corner across the line stays whole.
    int origin = -1;
    for (int k = 0; k < count; ++k)
        if (sideAt(k, kTrackCornerRadiusM) == 0) { origin = k; break; }
    if (origin < 0) return result;
    struct Run { int first; int last; int side; };   // offsets from origin
    std::vector<Run> runs;
    for (int step = 1; step < count;) {
        const int side = sideAt(origin + step, kTrackCornerRadiusM);
        if (side == 0) { ++step; continue; }
        int end = step;
        while (end + 1 < count && sideAt(origin + end + 1, kTrackCornerRadiusM) == side) ++end;
        if (!runs.empty() && runs.back().side == side && step - runs.back().last <= kTrackCornerJoinM)
            runs.back().last = end;
        else
            runs.push_back({step, end, side});
        step = end + 1;
    }
    runs.erase(std::remove_if(runs.begin(), runs.end(), [](const Run& run) {
        return run.last - run.first + 1 < kTrackCornerMinM;
    }), runs.end());
    // Neighbouring corners share the stretch between them at its straightest
    // point: neither runs past it.
    const auto straightness = [&](int k) { return std::abs(curvature[static_cast<size_t>(wrap(origin + k))]); };
    std::vector<int> split(runs.size() + 1, 0);
    split.back() = count;
    for (size_t r = 1; r < runs.size(); ++r) {
        int straightest = runs[r - 1].last + 1;
        for (int k = straightest; k < runs[r].first; ++k)
            if (straightness(k) < straightness(straightest)) straightest = k;
        split[r] = straightest;
    }
    for (size_t r = 0; r < runs.size(); ++r) {
        const auto& run = runs[r];
        // Out to where it is no tighter than kTrackCornerEndRadiusM.
        int first = run.first, last = run.last;
        while (first - 1 > split[r] && sideAt(origin + first - 1, kTrackCornerEndRadiusM) == run.side) --first;
        while (last + 1 < split[r + 1] && sideAt(origin + last + 1, kTrackCornerEndRadiusM) == run.side) ++last;
        // The apex: halfway through the turning of the corner's core.
        const auto turning = [&](int k) { return curvature[static_cast<size_t>(wrap(origin + k))] * run.side; };
        double peak = 0.0;
        for (int k = first; k <= last; ++k) peak = std::max(peak, turning(k));
        const auto core = [&](int k) { return turning(k) >= kApexCoreShare * peak ? turning(k) : 0.0; };
        double total = 0.0;
        for (int k = first; k <= last; ++k) total += core(k);
        int apex = first;
        for (double sum = 0.0; apex < last && sum + core(apex) < total / 2.0; ++apex) sum += core(apex);
        result.corners.push_back({static_cast<float>(wrap(origin + first)), static_cast<float>(wrap(origin + last)),
                                  static_cast<float>(wrap(origin + apex)), run.side});
    }
    std::sort(result.corners.begin(), result.corners.end(),
        [](const TrackCorner& x, const TrackCorner& y) { return x.apexM < y.apexM; });
    return result;
}

bool TrackCornerSweep::waitFor(TrackCorners& out) {
    std::unique_lock<std::mutex> lock(mutex_);
    changed_.wait(lock, [&] { return cancelled_.load() || done_; });
    if (cancelled_.load() || !corners_ || corners_->corners.empty()) return false;
    out = *corners_;
    return true;
}

void TrackCornerSweep::publish(std::optional<TrackCorners> corners) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        done_ = true;
        corners_ = std::move(corners);
    }
    changed_.notify_all();
}

void TrackCornerSweep::cancel() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cancelled_.store(true);
    }
    changed_.notify_all();
}

ErsPeakSweep::ErsPeakSweep(const std::vector<int>& drivers)
    : pending_(drivers.begin(), drivers.end()) {}

bool ErsPeakSweep::waitFor(int driverIndex, ErsPeaks& out) {
    std::unique_lock<std::mutex> lock(mutex_);
    changed_.wait(lock, [&] { return cancelled_.load() || !pending_.count(driverIndex); });
    const auto it = peaks_.find(driverIndex);
    if (cancelled_.load() || it == peaks_.end()) return false;
    out = it->second;
    return true;
}

void ErsPeakSweep::publish(int driverIndex, std::optional<ErsPeaks> peaks) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.erase(driverIndex);
        if (peaks) peaks_[driverIndex] = *peaks;
    }
    changed_.notify_all();
}

void ErsPeakSweep::cancel() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cancelled_.store(true);
    }
    changed_.notify_all();
}

int trackStatusAt(const std::vector<TrackStatusChange>& timeline, float sessionTime) {
    const auto after = std::upper_bound(
        timeline.begin(), timeline.end(), sessionTime,
        [](float value, const TrackStatusChange& change) { return value < change.session_time; });
    // Before the first Session row the first known status applies.
    if (after == timeline.begin()) return timeline.empty() ? 0 : timeline.front().status;
    return std::prev(after)->status;
}

void accumulateErsPeaks(ErsPeaks& peaks, const std::vector<ErsSample>& samples,
                        float startSessionTime, float endSessionTime,
                        const std::vector<TrackStatusChange>& timeline) {
    for (const auto& sample : samples) {
        if (sample.session_time < startSessionTime || sample.session_time > endSessionTime ||
            !std::isfinite(sample.mguk_kw)) continue;
        float& peak = trackStatusAt(timeline, sample.session_time) == 1
            ? peaks.reduced_kw : peaks.normal_kw;
        peak = std::max(peak, sample.mguk_kw);
    }
}

TrackDominanceMap calculateTrackDominance(const AnalysisLapProgress& current,
                                          const AnalysisLapProgress& comparison) {
    TrackDominanceMap result;
    result.miniSectorM = kMiniSectorM;
    const auto times = sectorTimes(current, comparison, kMiniSectorM);
    if (times.current.empty()) return result;
    result.trackLengthM = times.length;
    std::vector<std::optional<int>> faster(times.current.size());
    for (size_t k = 0; k < faster.size(); ++k) {
        const double timeA = times.current[k];
        const double timeB = times.comparison[k];
        if (!std::isfinite(timeA) || !std::isfinite(timeB)) continue;
        faster[k] = timeA == timeB ? -1 : timeA < timeB ? 0 : 1;
    }
    fillGaps(faster);
    result.faster.reserve(faster.size());
    for (const auto& value : faster) result.faster.push_back(value.value_or(-1));
    return result;
}

void accumulateHarvestPeak(ErsPeaks& peaks, const std::vector<HarvestSample>& samples,
                           float startSessionTime, float endSessionTime) {
    // Energy gained over windows of at least kHarvestWindowS. A drop is the
    // per-lap counter resetting at the line and starts a new window.
    double energy = 0.0;
    double seconds = 0.0;
    for (size_t i = 0; i + 1 < samples.size(); ++i) {
        const float t0 = samples[i].session_time;
        const float t1 = samples[i + 1].session_time;
        if (t0 < startSessionTime || t1 > endSessionTime) continue;
        const double gained = samples[i + 1].harvested_j - samples[i].harvested_j;
        const double dt = static_cast<double>(t1) - t0;
        if (!(dt > 0.0)) continue;
        if (!(gained >= 0.0)) { energy = 0.0; seconds = 0.0; continue; }
        energy += gained;
        seconds += dt;
        if (seconds >= kHarvestWindowS) {
            peaks.harvest_kw = std::max(peaks.harvest_kw, static_cast<float>(energy / seconds / 1000.0));
            energy = 0.0;
            seconds = 0.0;
        }
    }
}

ErsHarvestMap calculateErsHarvest(const AnalysisLapProgress& lap,
                                  const std::vector<HarvestSample>& harvest,
                                  const std::vector<ErsSample>& ers,
                                  const std::vector<InputSample>& inputs,
                                  const ErsPeaks& carPeaks) {
    ErsHarvestMap result;
    result.lapNum = lap.lapNum;
    result.binM = kErsBinM;
    result.peakKw = carPeaks.harvest_kw;
    const auto progress = detail::buildProgressMap(lap);
    if (progress.points.empty()) return result;
    const float length = lapLength(lap, progress.maxDistance);
    if (!(length > 0.0f)) return result;
    result.trackLengthM = length;
    result.restricted = isRestricted(ers) && std::all_of(harvest.begin(), harvest.end(),
        [](const HarvestSample& sample) { return !(sample.harvested_j > 0.0); });
    if (result.restricted) return result;

    // Each bin's harvest power is the energy gained in it over the time spent
    // in it, so the counters' increments average out.
    const size_t count = static_cast<size_t>(std::max(1.0f, std::ceil(length / kErsBinM)));
    std::vector<double> energy(count, 0.0);
    std::vector<double> seconds(count, 0.0);
    std::vector<double> flatOutIdle(count, 0.0);   // full throttle, MGU-K not deploying
    // The pedal and MGU-K samples in force at each harvest interval, walked
    // forward with it.
    size_t input = 0, power = 0;
    for (size_t i = 0; i + 1 < harvest.size(); ++i) {
        const float t0 = harvest[i].session_time;
        const float t1 = harvest[i + 1].session_time;
        const float dt = t1 - t0;
        if (!(dt > 0.0f) || t0 < lap.startSessionTime || t1 > lap.endSessionTime) continue;
        const double gained = harvest[i + 1].harvested_j - harvest[i].harvested_j;
        const float middle = (t0 + t1) / 2.0f;
        const float distance = distanceAtTime(progress, middle);
        if (!(distance >= 0.0f)) continue;
        const size_t bin = std::min(count - 1, static_cast<size_t>(distance / kErsBinM));
        energy[bin] += gained >= 0.0 ? gained : 0.0;   // a drop is the line reset
        seconds[bin] += dt;
        while (input + 1 < inputs.size() && inputs[input + 1].session_time <= middle) ++input;
        while (power + 1 < ers.size() && ers[power + 1].session_time <= middle) ++power;
        const bool flatOut = !inputs.empty() && inputs[input].session_time <= middle &&
            inputs[input].throttle >= kFullThrottle;
        const bool idle = !ers.empty() && ers[power].session_time <= middle &&
            ers[power].mguk_kw < kSuperclipDeployKw;
        if (flatOut && idle) flatOutIdle[bin] += dt;
    }
    const double peakW = static_cast<double>(carPeaks.harvest_kw) * 1000.0;
    std::vector<std::optional<float>> levels(count);
    for (size_t k = 0; k < count; ++k)
        if (seconds[k] > 0.0)
            levels[k] = peakW > 0.0
                ? static_cast<float>(std::clamp(energy[k] / seconds[k] / peakW, 0.0, 1.0)) : 0.0f;
    fillGaps(levels);
    result.level.reserve(count);
    for (const auto& value : levels) result.level.push_back(value.value_or(0.0f));

    result.superclip.assign(count, 0);
    for (size_t k = 0; k < count; ++k)
        if (seconds[k] > 0.0 && flatOutIdle[k] >= kSuperclipShare * seconds[k] &&
            energy[k] / seconds[k] / 1000.0 >= kSuperclipHarvestKw) result.superclip[k] = 1;
    // Drop runs too short to be a straight: single-packet blips.
    for (size_t k = 0; k < count;) {
        if (!result.superclip[k]) { ++k; continue; }
        size_t end = k;
        while (end < count && result.superclip[end]) ++end;
        if (end - k < kSuperclipMinBins) std::fill(result.superclip.begin() + k, result.superclip.begin() + end, 0);
        k = end;
    }
    return result;
}

std::vector<BrakingPoint> findBrakingPoints(const AnalysisLapProgress& lap,
                                            const std::vector<InputSample>& samples,
                                            const TrackCorners* corners) {
    std::vector<BrakingPoint> points;
    const auto progress = detail::buildProgressMap(lap);
    if (progress.points.empty()) return points;

    // Each brake application long enough to count is a zone of its own,
    // running to full throttle, or to the next braking point when the car
    // never gets back to full throttle before it.
    std::vector<BrakeApplication> applications;
    for (const auto& application : brakeApplications(lap, samples))
        if (application.endTime - application.startTime >= kBrakeHoldS) applications.push_back(application);

    // Each gear's grip ratio, engine rpm per km/h, from this lap's on-throttle running.
    std::map<int, float> gripRatio;
    {
        std::map<int, std::vector<float>> ratios;
        for (const auto& sample : samples)
            if (sample.session_time >= lap.startSessionTime && sample.session_time <= lap.endSessionTime &&
                sample.gear > 0 && sample.throttle > kPedalPressed && sample.brake <= kPedalPressed &&
                sample.speed_kph > kRatioMinKph && sample.rpm > 0.0f)
                ratios[sample.gear].push_back(sample.rpm / sample.speed_kph);
        for (auto& [gear, values] : ratios) {
            if (values.size() < kRatioMinSamples) continue;
            const auto middle = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
            std::nth_element(values.begin(), middle, values.end());
            gripRatio[gear] = *middle;
        }
    }

    for (size_t zone = 0; zone < applications.size(); ++zone) {
        const auto& application = applications[zone];
        const size_t start = application.start;
        const auto& at = samples[start];
        BrakingPoint point;
        point.distanceM = distanceAtTime(progress, at.session_time);
        if (!(point.distanceM >= 0.0f)) continue;
        point.speedKph = at.speed_kph;
        const float brakeOffM = distanceAtTime(progress, application.endTime);
        if (brakeOffM >= point.distanceM) point.brakingDistanceM = brakeOffM - point.distanceM;
        // Coasting: back from the brake while both pedals are off.
        size_t coast = start;
        while (coast > 0 && samples[coast - 1].session_time >= lap.startSessionTime &&
               samples[coast - 1].throttle <= kPedalPressed && samples[coast - 1].brake <= kPedalPressed)
            --coast;
        const float coastS = at.session_time - samples[coast].session_time;
        if (coast < start && coastS >= kMinCoastS) {
            const float from = distanceAtTime(progress, samples[coast].session_time);
            if (from >= 0.0f && from <= point.distanceM) {
                point.coastS = coastS;
                point.coastM = point.distanceM - from;
            }
        }
        // The slowest point: from the brake until the throttle goes back on.
        // The exit is measured from there.
        size_t slowest = start;
        for (size_t i = start; i < samples.size() && samples[i].session_time <= lap.endSessionTime; ++i) {
            if (i > start && samples[i].throttle > kPedalPressed && samples[i].brake <= kPedalPressed) break;
            if (samples[i].speed_kph < samples[slowest].speed_kph) slowest = i;
        }
        const float slowestAt = samples[slowest].session_time;

        // Corner exit: from that slowest point to the next braking point (or
        // the end of the lap), back on the throttle, then flat out.
        const float exitEnd = zone + 1 < applications.size()
            ? applications[zone + 1].startTime : lap.endSessionTime;
        std::optional<size_t> throttleOn, fullThrottle;
        for (size_t i = slowest; i < samples.size() && samples[i].session_time < exitEnd; ++i) {
            const auto& sample = samples[i];
            if (!throttleOn && sample.throttle > kPedalPressed && sample.brake <= kPedalPressed) throttleOn = i;
            if (throttleOn && sample.throttle >= kFullThrottle) { fullThrottle = i; break; }
        }
        if (throttleOn) {
            const float onAt = samples[*throttleOn].session_time;
            point.throttleOnM = distanceAtTime(progress, onAt);
            point.throttleOnS = onAt - slowestAt;
            if (fullThrottle) {
                point.fullThrottleM = distanceAtTime(progress, samples[*fullThrottle].session_time);
                point.fullThrottleS = samples[*fullThrottle].session_time - onAt;
                if (point.fullThrottleM >= point.throttleOnM)
                    point.throttleToFullM = point.fullThrottleM - point.throttleOnM;
            }
            // Wheelspin from back on the throttle to shortly after flat out.
            const float spinEnd = std::min(exitEnd, fullThrottle
                ? samples[*fullThrottle].session_time + kExitTailS : exitEnd);
            float gearChangedAt = -std::numeric_limits<float>::infinity();
            float spinning = 0.0f, longest = 0.0f, peak = 0.0f;
            for (size_t i = *throttleOn; i + 1 < samples.size() && samples[i].session_time < spinEnd; ++i) {
                const auto& sample = samples[i];
                const float dt = samples[i + 1].session_time - sample.session_time;
                if (i > *throttleOn && sample.gear != samples[i - 1].gear) gearChangedAt = sample.session_time;
                const auto ratio = gripRatio.find(sample.gear);
                const bool settled = sample.session_time - gearChangedAt >= kGearSettleS;
                float slip = 0.0f;
                if (ratio != gripRatio.end() && settled && sample.speed_kph > kRatioMinKph && sample.rpm > 0.0f)
                    slip = sample.rpm / (sample.speed_kph * ratio->second) - 1.0f;
                if (slip >= kWheelspinSlip) {
                    spinning += std::max(0.0f, dt);
                    peak = std::max(peak, slip);
                    longest = std::max(longest, spinning);
                } else {
                    spinning = 0.0f;
                }
            }
            if (longest >= kWheelspinMinS) {
                point.wheelspin = true;
                point.wheelspinPct = peak * 100.0f;
                point.wheelspinS = longest;
            }
        }

        // Where the zone ends, and its corners from the brake to there; the
        // slowest of them is the zone's minimum speed.
        const float zoneEnd = fullThrottle ? samples[*fullThrottle].session_time : exitEnd;
        point.endM = std::max(point.distanceM, distanceAtTime(progress, zoneEnd));
        point.apexes = findCorners(progress, samples, at.session_time, zoneEnd, corners);
        if (point.apexes.empty()) {
            const float m = distanceAtTime(progress, slowestAt);
            point.apexes.push_back({m >= 0.0f ? m : point.distanceM, samples[slowest].speed_kph});
        }
        setMinimumFromApexes(point);
        points.push_back(point);
    }
    return points;
}

namespace {

// A lap's lift over a stretch where the other lap brakes: off the throttle
// without the brake, from the lift to flat out again. Nothing when the lap was
// flat out through it or only blipped the throttle. The lift is looked for from
// kLiftLookbackM before the stretch, but never before `notBeforeM`, where this
// lap's previous zone ended, so its own braking for the corner before is not
// mistaken for a lift.
std::optional<BrakingPoint> liftZone(const detail::ProgressMap& progress,
                                     const std::vector<InputSample>& samples, float fromM, float toM,
                                     float notBeforeM, const TrackCorners* corners) {
    const float t0 = timeAtDistance(progress, std::max({0.0f, fromM - kLiftLookbackM, notBeforeM}));
    const float t1 = timeAtDistance(progress, toM);
    if (!std::isfinite(t0) || !std::isfinite(t1)) return std::nullopt;
    std::optional<size_t> lift, back, full;
    for (size_t i = 0; i < samples.size(); ++i) {
        const auto& sample = samples[i];
        if (sample.session_time < t0) continue;
        if (sample.session_time > t1 && !lift) break;
        if (!lift) {
            if (sample.throttle <= kPedalPressed && sample.brake <= kPedalPressed) lift = i;
        } else if (!back) {
            if (sample.throttle > kPedalPressed) back = i;
        } else if (sample.throttle >= kFullThrottle) {
            full = i;
            break;
        }
    }
    if (!lift || !back) return std::nullopt;
    const auto& at = samples[*lift];
    const float liftS = samples[*back].session_time - at.session_time;
    float slowest = at.speed_kph;
    for (size_t i = *lift; i <= *back; ++i) slowest = std::min(slowest, samples[i].speed_kph);
    if (liftS < kLiftMinS && at.speed_kph - slowest < kApexProminenceKph) return std::nullopt;

    BrakingPoint point;
    point.liftOnly = true;
    point.distanceM = distanceAtTime(progress, at.session_time);
    if (!(point.distanceM >= 0.0f)) return std::nullopt;
    point.speedKph = at.speed_kph;
    point.coastS = liftS;
    const float backM = distanceAtTime(progress, samples[*back].session_time);
    point.coastM = backM >= point.distanceM ? backM - point.distanceM : 0.0f;
    point.throttleOnM = backM;
    if (full) {
        point.fullThrottleM = distanceAtTime(progress, samples[*full].session_time);
        point.fullThrottleS = samples[*full].session_time - samples[*back].session_time;
        if (point.fullThrottleM >= point.throttleOnM) point.throttleToFullM = point.fullThrottleM - point.throttleOnM;
    }
    point.endM = std::max(point.distanceM, full ? point.fullThrottleM : toM);
    point.apexes = findCorners(progress, samples, at.session_time,
                               full ? samples[*full].session_time : t1, corners);
    if (point.apexes.empty()) point.apexes.push_back({point.distanceM, slowest});
    setMinimumFromApexes(point);
    return point;
}

} // namespace

BrakingPointsMap calculateBrakingPoints(const AnalysisLapProgress& current,
                                        const std::vector<InputSample>& currentInputs,
                                        const AnalysisLapProgress* comparison,
                                        const std::vector<InputSample>* comparisonInputs,
                                        const TrackCorners* currentCorners,
                                        const TrackCorners* comparisonCorners) {
    BrakingPointsMap result;
    result.trackLengthM = current.trackLengthM;
    result.current = findBrakingPoints(current, currentInputs, currentCorners);
    if (!comparison || !comparisonInputs) return result;
    if (!(result.trackLengthM > 0.0f)) result.trackLengthM = comparison->trackLengthM;
    result.comparison = findBrakingPoints(*comparison, *comparisonInputs, comparisonCorners);

    const auto a = detail::buildProgressMap(current);
    const auto b = detail::buildProgressMap(*comparison);

    // Braking points pair one to one, closest first: within kBrakingPairM, and
    // then any left over whose zones overlap.
    std::vector<bool> currentTaken(result.current.size(), false);
    std::vector<bool> comparisonTaken(result.comparison.size(), false);
    std::vector<std::pair<size_t, size_t>> matched;
    const auto pairUp = [&](bool overlapOnly) {
        struct Candidate { float gap; size_t current; size_t comparison; };
        std::vector<Candidate> candidates;
        for (size_t i = 0; i < result.current.size(); ++i) {
            if (currentTaken[i]) continue;
            const auto& x = result.current[i];
            for (size_t j = 0; j < result.comparison.size(); ++j) {
                if (comparisonTaken[j]) continue;
                const auto& y = result.comparison[j];
                const float gap = std::abs(x.distanceM - y.distanceM);
                const bool near = overlapOnly
                    ? x.distanceM <= y.endM && y.distanceM <= x.endM
                    : gap <= kBrakingPairM;
                if (near) candidates.push_back({gap, i, j});
            }
        }
        std::sort(candidates.begin(), candidates.end(),
            [](const Candidate& x, const Candidate& y) { return x.gap < y.gap; });
        for (const auto& candidate : candidates) {
            if (currentTaken[candidate.current] || comparisonTaken[candidate.comparison]) continue;
            currentTaken[candidate.current] = comparisonTaken[candidate.comparison] = true;
            matched.push_back({candidate.current, candidate.comparison});
        }
    };
    pairUp(false);
    pairUp(true);

    // A zone only one lap brakes for: the other lap may have taken it on a
    // lift, after its own previous zone.
    const size_t currentBraked = result.current.size();
    const size_t comparisonBraked = result.comparison.size();
    const auto previousEnd = [](const std::vector<BrakingPoint>& points, size_t braked, float fromM) {
        float end = 0.0f;
        for (size_t k = 0; k < braked; ++k)
            if (points[k].distanceM < fromM) end = std::max(end, points[k].endM);
        return end;
    };
    for (size_t i = 0; i < currentBraked; ++i) {
        if (currentTaken[i]) continue;
        const float from = result.current[i].distanceM, to = result.current[i].endM;
        const float notBefore = previousEnd(result.comparison, comparisonBraked, from);
        if (auto lift = liftZone(b, *comparisonInputs, from, to, notBefore, comparisonCorners)) {
            matched.push_back({i, result.comparison.size()});
            result.comparison.push_back(std::move(*lift));
        }
    }
    for (size_t j = 0; j < comparisonBraked; ++j) {
        if (comparisonTaken[j]) continue;
        const float from = result.comparison[j].distanceM, to = result.comparison[j].endM;
        const float notBefore = previousEnd(result.current, currentBraked, from);
        if (auto lift = liftZone(a, currentInputs, from, to, notBefore, currentCorners)) {
            matched.push_back({result.current.size(), j});
            result.current.push_back(std::move(*lift));
        }
    }

    // Each lap's zones back in lap order, the pairs following them.
    const auto inLapOrder = [](std::vector<BrakingPoint>& points) {
        std::vector<size_t> order(points.size());
        for (size_t k = 0; k < order.size(); ++k) order[k] = k;
        std::stable_sort(order.begin(), order.end(),
            [&](size_t x, size_t y) { return points[x].distanceM < points[y].distanceM; });
        std::vector<size_t> position(points.size());
        std::vector<BrakingPoint> sorted;
        sorted.reserve(points.size());
        for (size_t k = 0; k < order.size(); ++k) {
            position[order[k]] = k;
            sorted.push_back(std::move(points[order[k]]));
        }
        points = std::move(sorted);
        return position;
    };
    const auto currentAt = inLapOrder(result.current);
    const auto comparisonAt = inLapOrder(result.comparison);

    // Both laps are timed over the shared zone.
    const auto timeOver = [](const detail::ProgressMap& progress, float from, float to) {
        const double seconds = detail::interpolateElapsed(progress, to) - detail::interpolateElapsed(progress, from);
        return std::isfinite(seconds) && seconds > 0.0 ? static_cast<float>(seconds) : -1.0f;
    };
    for (const auto& [ci, bi] : matched) {
        const auto& x = result.current[currentAt[ci]];
        const auto& y = result.comparison[comparisonAt[bi]];
        BrakingPointPair pair;
        pair.current = static_cast<int>(currentAt[ci]);
        pair.comparison = static_cast<int>(comparisonAt[bi]);
        pair.gapM = x.distanceM - y.distanceM;
        pair.sameBrakingPoint = !x.liftOnly && !y.liftOnly && std::abs(pair.gapM) <= kBrakingPairM;
        // From the earlier braking point to the later of the two zone ends.
        pair.fromM = std::min(x.distanceM, y.distanceM);
        pair.toM = std::max(x.endM, y.endM);
        result.pairs.push_back(pair);
    }
    // A zone stops where the next one brakes, so zones never overlap.
    std::sort(result.pairs.begin(), result.pairs.end(),
        [](const BrakingPointPair& x, const BrakingPointPair& y) { return x.fromM < y.fromM; });
    for (size_t k = 0; k + 1 < result.pairs.size(); ++k)
        result.pairs[k].toM = std::max(result.pairs[k].fromM,
                                       std::min(result.pairs[k].toM, result.pairs[k + 1].fromM));
    for (auto& pair : result.pairs) {
        auto& x = result.current[static_cast<size_t>(pair.current)];
        auto& y = result.comparison[static_cast<size_t>(pair.comparison)];
        // Both laps' corners over the shared zone, so they line up.
        const auto refind = [&](BrakingPoint& point, const detail::ProgressMap& progress,
                                const std::vector<InputSample>& samples, const TrackCorners* corners) {
            const float t0 = timeAtDistance(progress, pair.fromM);
            const float t1 = timeAtDistance(progress, pair.toM);
            if (!std::isfinite(t0) || !std::isfinite(t1)) return;
            auto apexes = findCorners(progress, samples, t0, t1, corners);
            if (apexes.empty()) return;
            point.apexes = std::move(apexes);
            setMinimumFromApexes(point);
        };
        refind(x, a, currentInputs, currentCorners);
        refind(y, b, *comparisonInputs, comparisonCorners);
        pair.currentTimeS = timeOver(a, pair.fromM, pair.toM);
        pair.comparisonTimeS = timeOver(b, pair.fromM, pair.toM);
        // Braking and acceleration, split at one distance for both laps so
        // their halves are timed over the same stretch.
        const float split = (x.minSpeedM + y.minSpeedM) / 2.0f;
        if (std::abs(x.minSpeedM - y.minSpeedM) <= kPhaseAgreeM &&
            split - pair.fromM >= kPhaseMinM && pair.toM - split >= kPhaseMinM) {
            pair.splitM = split;
            pair.currentBrakingS = timeOver(a, pair.fromM, split);
            pair.comparisonBrakingS = timeOver(b, pair.fromM, split);
            pair.currentAccelerationS = timeOver(a, split, pair.toM);
            pair.comparisonAccelerationS = timeOver(b, split, pair.toM);
        }
    }
    return result;
}

DriverInputsMap calculateDriverInputs(const AnalysisLapProgress& lap,
                                      const std::vector<InputSample>& samples) {
    DriverInputsMap result;
    result.lapNum = lap.lapNum;
    result.binM = kInputBinM;
    const auto progress = detail::buildProgressMap(lap);
    if (progress.points.empty() || samples.empty()) return result;
    const float length = lapLength(lap, progress.maxDistance);
    if (!(length > 0.0f)) return result;
    result.trackLengthM = length;

    // Each sample holds until the next one; each bin takes the state it held
    // for longest.
    const size_t count = static_cast<size_t>(std::max(1.0f, std::ceil(length / kInputBinM)));
    std::vector<std::array<double, 4>> held(count, std::array<double, 4>{});
    // Inside a brake application a moment off both pedals is an ABS pulse or
    // a pressure dip, still braking; a throttle blip there stays throttle.
    const auto applications = brakeApplications(lap, samples);
    size_t application = 0;
    for (size_t i = 0; i + 1 < samples.size(); ++i) {
        const float t0 = samples[i].session_time;
        const float t1 = samples[i + 1].session_time;
        const float dt = t1 - t0;
        if (!(dt > 0.0f) || t0 < lap.startSessionTime || t1 > lap.endSessionTime) continue;
        const float distance = distanceAtTime(progress, (t0 + t1) / 2.0f);
        if (!(distance >= 0.0f)) continue;
        const size_t bin = std::min(count - 1, static_cast<size_t>(distance / kInputBinM));
        while (application < applications.size() && applications[application].endTime < t0) ++application;
        PedalState state = pedalState(samples[i]);
        if (state == PedalState::Coasting && application < applications.size() &&
            applications[application].startTime <= t0) state = PedalState::Braking;
        held[bin][static_cast<size_t>(state)] += dt;
    }
    std::vector<std::optional<int>> states(count);
    for (size_t k = 0; k < count; ++k) {
        const auto longest = std::max_element(held[k].begin(), held[k].end());
        if (*longest > 0.0) states[k] = static_cast<int>(longest - held[k].begin());
    }
    fillGaps(states);
    result.state.reserve(count);
    for (const auto& value : states) result.state.push_back(value.value_or(static_cast<int>(PedalState::Coasting)));
    return result;
}

SpeedMap calculateSpeed(const AnalysisLapProgress& lap,
                        const std::vector<InputSample>& samples,
                        const TrackCorners* corners) {
    SpeedMap result;
    result.lapNum = lap.lapNum;
    result.binM = kSpeedBinM;
    const auto progress = detail::buildProgressMap(lap);
    if (progress.points.empty() || samples.empty()) return result;
    const float length = lapLength(lap, progress.maxDistance);
    if (!(length > 0.0f)) return result;
    result.trackLengthM = length;

    // Each sample holds until the next one; its time in a bin weights it.
    const size_t count = static_cast<size_t>(std::max(1.0f, std::ceil(length / kSpeedBinM)));
    std::vector<double> sum(count, 0.0);
    std::vector<double> weight(count, 0.0);
    float lowest = std::numeric_limits<float>::infinity();
    float highest = -std::numeric_limits<float>::infinity();
    for (size_t i = 0; i + 1 < samples.size(); ++i) {
        const float t0 = samples[i].session_time;
        const float t1 = samples[i + 1].session_time;
        const float dt = t1 - t0;
        if (!(dt > 0.0f) || t0 < lap.startSessionTime || t1 > lap.endSessionTime) continue;
        const float speed = samples[i].speed_kph;
        if (!(speed >= 0.0f)) continue;
        const float distance = distanceAtTime(progress, (t0 + t1) / 2.0f);
        if (!(distance >= 0.0f)) continue;
        const size_t bin = std::min(count - 1, static_cast<size_t>(distance / kSpeedBinM));
        sum[bin] += static_cast<double>(speed) * dt;
        weight[bin] += dt;
        lowest = std::min(lowest, speed);
        highest = std::max(highest, speed);
    }
    if (!(highest >= lowest)) return result;
    result.minKph = lowest;
    result.maxKph = highest;
    std::vector<std::optional<float>> speeds(count);
    for (size_t k = 0; k < count; ++k)
        if (weight[k] > 0.0) speeds[k] = static_cast<float>(sum[k] / weight[k]);
    fillGaps(speeds);
    result.speedKph.reserve(count);
    for (const auto& value : speeds) result.speedKph.push_back(value.value_or(lowest));
    // A track corner two zones both reach is listed once.
    for (const auto& zone : findBrakingPoints(lap, samples, corners))
        for (const auto& apex : zone.apexes)
            if (apex.side == 0 || std::none_of(result.apexes.begin(), result.apexes.end(),
                    [&](const CornerApex& listed) { return listed.distanceM == apex.distanceM; }))
                result.apexes.push_back(apex);
    return result;
}

ErsUsageMap calculateErsUsage(const AnalysisLapProgress& lap,
                              const std::vector<ErsSample>& samples,
                              const std::vector<TrackStatusChange>& timeline,
                              const ErsPeaks& carPeaks) {
    ErsUsageMap result;
    result.lapNum = lap.lapNum;
    result.binM = kErsBinM;
    result.normalPeakKw = carPeaks.normal_kw;
    result.reducedPeakKw = carPeaks.reduced_kw;
    const auto progress = detail::buildProgressMap(lap);
    if (progress.points.empty()) return result;
    const float length = lapLength(lap, progress.maxDistance);
    if (!(length > 0.0f)) return result;
    result.trackLengthM = length;
    result.restricted = isRestricted(samples);
    if (result.restricted) return result;

    // Each sample holds until the next one; its time in a bin weights it.
    const size_t count = static_cast<size_t>(std::max(1.0f, std::ceil(length / kErsBinM)));
    std::vector<double> sum(count, 0.0);
    std::vector<double> weight(count, 0.0);
    for (size_t i = 0; i + 1 < samples.size(); ++i) {
        const auto& sample = samples[i];
        const float t0 = sample.session_time;
        const float t1 = samples[i + 1].session_time;
        const float dt = t1 - t0;
        if (!(dt > 0.0f) || t0 < lap.startSessionTime || t1 > lap.endSessionTime) continue;
        const bool reduced = trackStatusAt(timeline, t0) == 1;
        (reduced ? result.usesReduced : result.usesNormal) = true;
        const float peak = reduced ? carPeaks.reduced_kw : carPeaks.normal_kw;
        const float level = peak > 0.0f
            ? std::clamp(std::max(0.0f, sample.mguk_kw) / peak, 0.0f, 1.0f) : 0.0f;
        const float distance = distanceAtTime(progress, (t0 + t1) / 2.0f);
        if (!(distance >= 0.0f)) continue;
        const size_t bin = std::min(count - 1, static_cast<size_t>(distance / kErsBinM));
        sum[bin] += level * dt;
        weight[bin] += dt;
    }
    std::vector<std::optional<float>> levels(count);
    for (size_t k = 0; k < count; ++k)
        if (weight[k] > 0.0) levels[k] = static_cast<float>(sum[k] / weight[k]);
    fillGaps(levels);
    result.level.reserve(count);
    for (const auto& value : levels) result.level.push_back(value.value_or(0.0f));
    return result;
}

AnalysisMapResult buildAnalysisMap(const std::string& mode, const AnalysisMapLap& current,
                                   const AnalysisMapLap* comparison) {
    AnalysisMapResult result;
    result.mode = mode;
    const bool harvest = mode == "harvest";
    const bool ers = mode == "ers" || harvest;
    const bool braking = mode == "braking";
    const bool inputs = mode == "inputs";
    const bool speed = mode == "speed";
    if (!ers && !inputs && !speed && !braking && mode != "dominance") return result;

    // The pedals feed Driver Inputs, Speed (and its corners), braking points
    // and superclip detection.
    const bool readPedals = inputs || speed || braking || harvest;
    // Braking zones read each lap's ERS and harvest samples but need no peaks.
    const bool readErs = ers || braking;
    const bool readHarvest = harvest || braking;
    LapRead a = readLap(current, readErs, readPedals, readHarvest, !braking);
    std::optional<LapRead> b = comparison
        ? std::optional<LapRead>(readLap(*comparison, readErs, readPedals, readHarvest, !braking)) : std::nullopt;
    result.hasCurrent = a.ok;
    result.hasComparison = b && b->ok;
    // Speed and braking place each corner from the lap's own recording.
    if (speed || braking) {
        if (result.hasCurrent) a.corners = readCorners(current);
        if (result.hasComparison) b->corners = readCorners(*comparison);
    }
    const TrackCorners* currentCorners = a.corners ? &*a.corners : nullptr;
    const TrackCorners* comparisonCorners = b && b->corners ? &*b->corners : nullptr;

    if (braking) {
        if (result.hasCurrent)
            result.braking = calculateBrakingPoints(a.progress, a.inputs,
                result.hasComparison ? &b->progress : nullptr,
                result.hasComparison ? &b->inputs : nullptr,
                currentCorners, comparisonCorners);
        if (result.hasCurrent && result.hasComparison) {
            const auto readLine = [](LapRead& lap, const AnalysisMapLap& request) {
                (void)request.recording([&](const TnrdReader& reader) {
                    (void)reader.getAnalysisPositionSamples(request.lapNum, lap.positions, request.driverIndex);
                    lap.trackLimits = reader.trackLimitTimes(request.driverIndex);
                });
            };
            readLine(a, current);
            readLine(*b, *comparison);
            describeBrakingZones(result.braking, a, *b);
        }
        return result;
    }
    if (inputs) {
        if (result.hasCurrent) result.currentInputs = calculateDriverInputs(a.progress, a.inputs);
        if (result.hasComparison) result.comparisonInputs = calculateDriverInputs(b->progress, b->inputs);
        return result;
    }
    if (speed) {
        if (result.hasCurrent) result.currentSpeed = calculateSpeed(a.progress, a.inputs, currentCorners);
        if (result.hasComparison)
            result.comparisonSpeed = calculateSpeed(b->progress, b->inputs, comparisonCorners);
        bool scaled = false;
        for (const SpeedMap* lap : {&result.currentSpeed, &result.comparisonSpeed}) {
            if (lap->speedKph.empty()) continue;
            result.speedScaleMinKph = scaled ? std::min(result.speedScaleMinKph, lap->minKph) : lap->minKph;
            result.speedScaleMaxKph = scaled ? std::max(result.speedScaleMaxKph, lap->maxKph) : lap->maxKph;
            scaled = true;
        }
        return result;
    }
    if (!ers) {
        if (result.hasCurrent && result.hasComparison)
            result.dominance = calculateTrackDominance(a.progress, b->progress);
        return result;
    }
    if (harvest) {
        if (result.hasCurrent)
            result.currentHarvest = calculateErsHarvest(a.progress, a.harvest, a.ers, a.inputs, a.peaks);
        if (result.hasComparison)
            result.comparisonHarvest = calculateErsHarvest(b->progress, b->harvest, b->ers, b->inputs, b->peaks);
        result.harvestPeakMismatch = result.hasCurrent && result.hasComparison &&
            !result.currentHarvest.restricted && !result.comparisonHarvest.restricted &&
            std::abs(result.currentHarvest.peakKw - result.comparisonHarvest.peakKw) > kPeakMismatchKw;
        return result;
    }
    if (result.hasCurrent) result.current = calculateErsUsage(a.progress, a.ers, a.trackStatus, a.peaks);
    if (result.hasComparison) result.comparison = calculateErsUsage(b->progress, b->ers, b->trackStatus, b->peaks);
    if (result.hasCurrent && result.hasComparison &&
        !result.current.restricted && !result.comparison.restricted) {
        const auto& x = result.current;
        const auto& y = result.comparison;
        result.normalPeakMismatch = x.usesNormal && y.usesNormal &&
            std::abs(x.normalPeakKw - y.normalPeakKw) > kPeakMismatchKw;
        result.reducedPeakMismatch = x.usesReduced && y.usesReduced &&
            std::abs(x.reducedPeakKw - y.reducedPeakKw) > kPeakMismatchKw;
    }
    return result;
}

} // namespace tnrp
