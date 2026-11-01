#include "tnrp/AnalysisMap.h"

#include "LapProgressMap.h"
#include "tnrp/TnrdReader.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <limits>
#include <map>
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
constexpr size_t kMaxBrakingObservations = 4;
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

// The corners whose turn-in falls between two session times, from the
// steering: where the wheel is turned furthest into the turn, and the slowest speed through
// each. Without steering, speed dips stand in (findApexes).
std::vector<CornerApex> findApexes(const detail::ProgressMap& progress,
                                   const std::vector<InputSample>& samples, float from, float to);
std::vector<CornerApex> findCorners(const detail::ProgressMap& progress,
                                    const std::vector<InputSample>& samples, float from, float to) {
    const auto corners = steeringCorners(samples);
    if (corners.empty()) return findApexes(progress, samples, from, to);
    std::vector<CornerApex> out;
    for (const auto& corner : corners) {
        size_t peak = corner.first;
        float slowest = samples[corner.first].speed_kph;
        for (size_t i = corner.first; i <= corner.last; ++i) {
            if (samples[i].steering * corner.side > samples[peak].steering * corner.side) peak = i;
            slowest = std::min(slowest, samples[i].speed_kph);
        }
        const float at = samples[peak].session_time;
        if (at < from || at > to) continue;
        const float m = distanceAtTime(progress, at);
        if (m >= 0.0f) out.push_back({m, slowest, corner.side});
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
    bool ok{};
};

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

// Each matched braking zone's ERS figures and observations.
void describeBrakingZones(BrakingPointsMap& braking, const LapRead& current, const LapRead& comparison) {
    for (auto& pair : braking.pairs) {
        // Harvest is under braking: over the braking half when the zone splits.
        const float harvestTo = pair.splitM >= 0.0f ? pair.splitM : pair.toM;
        std::tie(pair.currentHarvestKj, pair.currentStoreMj) = zoneEnergy(current, pair.fromM, harvestTo);
        std::tie(pair.comparisonHarvestKj, pair.comparisonStoreMj) = zoneEnergy(comparison, pair.fromM, harvestTo);
        const auto& a = braking.current[static_cast<size_t>(pair.current)];
        const auto& b = braking.comparison[static_cast<size_t>(pair.comparison)];
        auto& out = pair.observations;
        const bool timed = pair.currentTimeS >= 0.0f && pair.comparisonTimeS >= 0.0f;
        const int quicker = !timed || pair.currentTimeS == pair.comparisonTimeS ? -1
            : pair.currentTimeS < pair.comparisonTimeS ? 0 : 1;
        const float gainS = timed ? std::abs(pair.comparisonTimeS - pair.currentTimeS) : 0.0f;
        const auto& quick = quicker == 0 ? a : b;
        const auto& slow = quicker == 0 ? b : a;
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
                   o.kind == "earlier_throttle" || o.kind == "faster_to_full";
        };
        size_t entries = 0, exits = 0;
        out.erase(std::remove_if(out.begin(), out.end(), [&](const BrakingObservation& o) {
            return ++(exitKind(o) ? exits : entries) > kMaxBrakingObservations;
        }), out.end());
    }
}

} // namespace

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
                                            const std::vector<InputSample>& samples) {
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
        point.apexes = findCorners(progress, samples, at.session_time, zoneEnd);
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
                                     float notBeforeM) {
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
                               full ? samples[*full].session_time : t1);
    if (point.apexes.empty()) point.apexes.push_back({point.distanceM, slowest});
    setMinimumFromApexes(point);
    return point;
}

} // namespace

BrakingPointsMap calculateBrakingPoints(const AnalysisLapProgress& current,
                                        const std::vector<InputSample>& currentInputs,
                                        const AnalysisLapProgress* comparison,
                                        const std::vector<InputSample>* comparisonInputs) {
    BrakingPointsMap result;
    result.trackLengthM = current.trackLengthM;
    result.current = findBrakingPoints(current, currentInputs);
    if (!comparison || !comparisonInputs) return result;
    if (!(result.trackLengthM > 0.0f)) result.trackLengthM = comparison->trackLengthM;
    result.comparison = findBrakingPoints(*comparison, *comparisonInputs);

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
        if (auto lift = liftZone(b, *comparisonInputs, from, to, notBefore)) {
            matched.push_back({i, result.comparison.size()});
            result.comparison.push_back(std::move(*lift));
        }
    }
    for (size_t j = 0; j < comparisonBraked; ++j) {
        if (comparisonTaken[j]) continue;
        const float from = result.comparison[j].distanceM, to = result.comparison[j].endM;
        const float notBefore = previousEnd(result.current, currentBraked, from);
        if (auto lift = liftZone(a, currentInputs, from, to, notBefore)) {
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
                                const std::vector<InputSample>& samples) {
            const float t0 = timeAtDistance(progress, pair.fromM);
            const float t1 = timeAtDistance(progress, pair.toM);
            if (!std::isfinite(t0) || !std::isfinite(t1)) return;
            auto apexes = findCorners(progress, samples, t0, t1);
            if (apexes.empty()) return;
            point.apexes = std::move(apexes);
            setMinimumFromApexes(point);
        };
        refind(x, a, currentInputs);
        refind(y, b, *comparisonInputs);
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
    if (!ers && !inputs && !braking && mode != "dominance") return result;

    // The pedals feed Driver Inputs, braking points and superclip detection.
    const bool readPedals = inputs || braking || harvest;
    // Braking zones read each lap's ERS and harvest samples but need no peaks.
    const bool readErs = ers || braking;
    const bool readHarvest = harvest || braking;
    const LapRead a = readLap(current, readErs, readPedals, readHarvest, !braking);
    const std::optional<LapRead> b = comparison
        ? std::optional<LapRead>(readLap(*comparison, readErs, readPedals, readHarvest, !braking)) : std::nullopt;
    result.hasCurrent = a.ok;
    result.hasComparison = b && b->ok;

    if (braking) {
        if (result.hasCurrent)
            result.braking = calculateBrakingPoints(a.progress, a.inputs,
                result.hasComparison ? &b->progress : nullptr,
                result.hasComparison ? &b->inputs : nullptr);
        if (result.hasCurrent && result.hasComparison) describeBrakingZones(result.braking, a, *b);
        return result;
    }
    if (inputs) {
        if (result.hasCurrent) result.currentInputs = calculateDriverInputs(a.progress, a.inputs);
        if (result.hasComparison) result.comparisonInputs = calculateDriverInputs(b->progress, b->inputs);
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
