#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "tnrp/LapDelta.h"

namespace tnrp {

class TnrdReader;

// Analysis map colouring. Hosts name the laps and draw the result; libtnrp
// owns which lap is faster where and how hard each car deployed ERS.

// One Car Status sample: MGU-K output in kW (zero while harvesting) and the
// ERS store, which Restricted telemetry also reports as zero.
struct ErsSample {
    float session_time{};
    float mguk_kw{};
    double ers_j{};
};

// One Car Status sample of the per-lap harvest counters: MGU-K plus MGU-H
// energy harvested so far this lap, in joules. It resets at the line.
struct HarvestSample {
    float session_time{};
    double harvested_j{};
};

// One Car Telemetry sample of the pedals (each 0-1), the car's speed, the
// engine speed and the gear (0 neutral, -1 reverse).
struct InputSample {
    float session_time{};
    float throttle{};
    float brake{};
    float speed_kph{};
    float rpm{};
    int gear{};
    float steering{};   // -1 full left to 1 full right
};

// The 2026 active-aero track status in force from `session_time` on:
// 0 = Full (normal grip), 1 = Partial (reduced grip). Recordings without the
// field have none, which reads as Full throughout.
struct TrackStatusChange {
    float session_time{};
    int status{};
};

// A car's highest MGU-K output (kW) anywhere in its recording, split by the
// grip in force when it was reached. Zero when the car never deployed there.
struct ErsPeaks {
    float normal_kw{};
    float reduced_kw{};
    // Highest harvest power (kW), held for at least kHarvestWindowS.
    float harvest_kw{};
};

// Every car's ERS peaks, found by a background sweep started when a recording
// opens (TnrdReader::startErsPeakSweep) and published car by car. Thread-safe.
class ErsPeakSweep {
public:
    explicit ErsPeakSweep(const std::vector<int>& drivers);
    // Blocks until that car's peaks are known. False when the sweep does not
    // cover the car, could not read it, or was cancelled.
    bool waitFor(int driverIndex, ErsPeaks& out);
    // Records a car's result; nullopt when it could not be read.
    void publish(int driverIndex, std::optional<ErsPeaks> peaks);
    void cancel();
    bool cancelled() const { return cancelled_.load(); }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::set<int> pending_;
    std::map<int, ErsPeaks> peaks_;
    std::atomic<bool> cancelled_{false};
};

// One corner of the track, found from every car's laps rather than one lap's
// steering: where the path averaged over them turns tighter than a corner.
struct TrackCorner {
    float fromM{};   // lap distance where it starts turning
    float toM{};     // ... and stops; below fromM when it crosses the line
    float apexM{};   // halfway through the turning of its tightest stretch
    int side{};      // -1 left, 1 right
};

struct TrackCorners {
    float trackLengthM{};
    std::vector<TrackCorner> corners;
};

// The path of many laps averaged by lap distance, metre by metre, and the
// corners in it. Laps can come from any car; add each clean one.
class TrackPathAverage {
public:
    explicit TrackPathAverage(float trackLengthM);
    // One lap: its world positions and its lap distance over time, each in
    // time order. False when it was not used: too few samples, or the car
    // jumped (a teleport back to the track).
    bool addLap(const std::vector<float>& time, const std::vector<double>& x,
                const std::vector<double>& z,
                const std::vector<std::pair<float, double>>& lapDistance);
    void merge(const TrackPathAverage& other);
    size_t laps() const { return laps_; }
    // Empty when too few laps were added or they leave gaps in the lap.
    TrackCorners corners() const;

private:
    float trackLengthM_{};
    size_t laps_{};
    std::vector<double> sumX_;
    std::vector<double> sumZ_;
    std::vector<uint32_t> count_;
};

// A recording's track corners, found by a background sweep over every car's
// laps started when it opens (TnrdReader::startTrackCornerSweep). Thread-safe.
class TrackCornerSweep {
public:
    // Blocks until the sweep finishes. False when it found no corners or
    // was cancelled.
    bool waitFor(TrackCorners& out);
    void publish(std::optional<TrackCorners> corners);
    void cancel();
    bool cancelled() const { return cancelled_.load(); }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool done_{};
    std::optional<TrackCorners> corners_;
    std::atomic<bool> cancelled_{false};
};

// Track Dominance: the lap is cut into equal mini-sectors from the timing line.
struct TrackDominanceMap {
    float miniSectorM{};
    float trackLengthM{};
    // Per mini-sector: 0 = the current lap was faster, 1 = the comparison lap,
    // -1 = level. A mini-sector neither lap covers takes its neighbour's value.
    std::vector<int> faster;
};

// What the driver was doing with the pedals, per stretch of track.
enum class PedalState : int { Coasting = 0, PartialThrottle = 1, FullThrottle = 2, Braking = 3 };

// Driver inputs for one lap, in fixed-length bins from the timing line.
struct DriverInputsMap {
    int lapNum{};
    float binM{};
    float trackLengthM{};
    // Per bin: the PedalState held longest there. Bins no sample landed in
    // take a neighbour's value.
    std::vector<int> state;
};

// Something worth pointing out about a braking zone, as facts for hosts to
// word. `lap` is 0 for the current lap and 1 for the comparison lap; `value`
// and `other` are that lap's and the other lap's figures, in `kind`'s units:
//   quicker_despite_coast  value = s quicker through the zone, other = s longer coasting
//   apex_speed             value / other = apex km/h of `lap` / the other lap
//   later_but_slower       value = m later on the brake, other = s slower through the zone
//   more_harvest           value / other = kJ harvested in the zone by `lap` / the other lap
//   low_battery            value = MJ in the store as `lap` enters the zone
//   wheelspin              value = peak slip %, other = s spinning on the exit
//   earlier_throttle_spin  value = m earlier on the throttle, other = peak slip %
//   earlier_throttle       value = m earlier on the throttle, without wheelspin
//   faster_to_full         value = m earlier to full throttle, without wheelspin
//   lift_only              `lap` takes the zone on a lift where the other lap brakes
struct BrakingObservation {
    std::string kind;
    int lap{};
    float value{};
    float other{};
};

// Two laps' braking points matched up: indices into BrakingPointsMap's lists.
struct BrakingPointPair {
    int current{};
    int comparison{};
    // Metres: positive where the current lap started braking later. Only
    // meaningful when sameBrakingPoint: one lap may take it on a lift.
    float gapM{};
    bool sameBrakingPoint{};
    // The zone both laps are timed over: from the earlier braking point to the
    // later of the two laps' zone ends, cut off at the next zone's start.
    float fromM{};
    float toM{};
    // Seconds each lap took over it; -1 when a lap does not cover it.
    float currentTimeS{-1.0f};
    float comparisonTimeS{-1.0f};
    // Where the zone splits into braking (fromM to splitM) and acceleration
    // (splitM to toM): halfway between the two laps' slowest points. -1 when
    // the zone is not split: those points are far apart, or a half would be
    // too short to time. Each lap's seconds over each half, -1 when unsplit.
    float splitM{-1.0f};
    float currentBrakingS{-1.0f};
    float comparisonBrakingS{-1.0f};
    float currentAccelerationS{-1.0f};
    float comparisonAccelerationS{-1.0f};
    // ERS harvested over the zone's braking half, or the whole zone when it is
    // not split (kJ), and the store as each lap enters it
    // (MJ); -1 when unknown, as for a car with Restricted telemetry.
    float currentHarvestKj{-1.0f};
    float comparisonHarvestKj{-1.0f};
    float currentStoreMj{-1.0f};
    float comparisonStoreMj{-1.0f};
    // At most kMaxBrakingObservations for the braking half and as many for the
    // corner exit (wheelspin, throttle on, flat out), the most telling first.
    std::vector<BrakingObservation> observations;
};

// One corner inside a braking zone, flat-out ones included: its apex, the
// slowest speed through it, and which way it turns (-1 left, 1 right; 0 when
// found from speed alone). With the recording's TrackCorners the apex is the
// track's; without them it is found from the lap's steering, where the driver
// has done half the corner's steering, or from speed dips with no steering.
struct CornerApex {
    float distanceM{};
    float speedKph{};
    int side{};
};

// One braking zone of one lap: from a braking point to full throttle, or to
// the next braking point when the car never gets back to full throttle first.
struct BrakingPoint {
    float endM{};           // lap distance where the zone ends
    // Taken on a lift, without the brake: matched to a zone the other lap
    // brakes for. distanceM is then where the throttle came off, and the
    // coasting is the whole lift.
    bool liftOnly{};
    float distanceM{};      // lap distance where the brake goes on
    float speedKph{};       // speed there
    // Coasting into it: off both pedals right before braking. Zero when the
    // driver went straight from throttle to brake.
    float coastM{};
    float coastS{};
    // From the brake going on to it coming off.
    float brakingDistanceM{-1.0f};
    float minSpeedKph{};    // the lowest speed in the zone
    float minSpeedM{};      // lap distance of that lowest speed
    // Each corner in the zone, in order, flat-out ones included.
    std::vector<CornerApex> apexes;
    // Corner exit, from the slowest point on; -1 where it never happened
    // before the next braking zone or the end of the lap.
    float throttleOnM{-1.0f};     // back on the throttle
    float throttleOnS{-1.0f};     // ... this long after the slowest point
    float fullThrottleM{-1.0f};   // first at full throttle
    float fullThrottleS{-1.0f};   // ... this long after getting back on it
    float throttleToFullM{-1.0f}; // distance from back on the throttle to flat out
    // Rear wheelspin on the exit, read from the engine speed running ahead of
    // the road speed for the gear: the worst slip (%) and how long it lasted.
    bool wheelspin{};
    float wheelspinPct{};
    float wheelspinS{};
};

// Where each lap starts braking for each corner.
struct BrakingPointsMap {
    float trackLengthM{};
    std::vector<BrakingPoint> current;
    std::vector<BrakingPoint> comparison;
    std::vector<BrakingPointPair> pairs;
};

// ERS Harvest for one lap, in fixed-length bins from the timing line.
struct ErsHarvestMap {
    int lapNum{};
    float binM{};
    float trackLengthM{};
    // Per bin: harvest power as a fraction (0-1) of the car's peak harvest
    // power. Bins no sample landed in take a neighbour's value.
    std::vector<float> level;
    // Per bin: 1 where the car was superclipping, harvesting while flat out
    // with the MGU-K not deploying. Superclip harvest is a small fraction of
    // braking harvest, so it is marked as a state rather than left to `level`.
    std::vector<int> superclip;
    // Every ERS field read zero: the driver's telemetry is Restricted.
    bool restricted{};
    float peakKw{};
};

// Speed for one lap, in fixed-length bins from the timing line.
struct SpeedMap {
    int lapNum{};
    float binM{};
    float trackLengthM{};
    // Per bin: the time-weighted mean speed (km/h). Bins no sample landed in
    // take a neighbour's value.
    std::vector<float> speedKph;
    // The lap's slowest and fastest samples (km/h).
    float minKph{};
    float maxKph{};
    // Each corner in the lap's braking zones (findBrakingPoints), with its
    // slowest speed, in lap order.
    std::vector<CornerApex> apexes;
};

// ERS Usage for one lap, in fixed-length bins from the timing line.
struct ErsUsageMap {
    int lapNum{};
    float binM{};
    float trackLengthM{};
    // Per bin: MGU-K output as a fraction (0-1) of the car's peak for the grip
    // in force there. Bins no sample landed in take a neighbour's value.
    std::vector<float> level;
    // Every ERS field read zero: the driver's telemetry is Restricted.
    bool restricted{};
    // Which grip buckets the lap was driven in, and the car's peaks for them.
    bool usesNormal{};
    bool usesReduced{};
    float normalPeakKw{};
    float reducedPeakKw{};
};

struct AnalysisMapResult {
    std::string mode;   // "dominance", "ers", "harvest", "inputs", "speed" or "braking"
    TrackDominanceMap dominance;
    SpeedMap currentSpeed;
    SpeedMap comparisonSpeed;
    // Speed with two laps: one scale (km/h) covering both, so a colour means
    // the same speed on either map. One lap alone: that lap's own range.
    float speedScaleMinKph{};
    float speedScaleMaxKph{};
    ErsUsageMap current;
    ErsUsageMap comparison;
    DriverInputsMap currentInputs;
    DriverInputsMap comparisonInputs;
    ErsHarvestMap currentHarvest;
    ErsHarvestMap comparisonHarvest;
    BrakingPointsMap braking;
    bool hasCurrent{};
    bool hasComparison{};
    // ERS Usage with two laps: their cars' peaks differ by more than reporting
    // noise in a grip bucket both laps were driven in.
    bool normalPeakMismatch{};
    bool reducedPeakMismatch{};
    // ERS Harvest with two laps: their cars' peak harvest power differs.
    bool harvestPeakMismatch{};
};

int trackStatusAt(const std::vector<TrackStatusChange>& timeline, float sessionTime);

// Adds samples taken between `startSessionTime` and `endSessionTime` to a
// car's peaks, each in the grip bucket in force at its time.
void accumulateErsPeaks(ErsPeaks& peaks, const std::vector<ErsSample>& samples,
                        float startSessionTime, float endSessionTime,
                        const std::vector<TrackStatusChange>& timeline);

TrackDominanceMap calculateTrackDominance(const AnalysisLapProgress& current,
                                          const AnalysisLapProgress& comparison);

// Raises a car's peak harvest power with samples taken between
// `startSessionTime` and `endSessionTime`.
void accumulateHarvestPeak(ErsPeaks& peaks, const std::vector<HarvestSample>& samples,
                           float startSessionTime, float endSessionTime);

ErsHarvestMap calculateErsHarvest(const AnalysisLapProgress& lap,
                                  const std::vector<HarvestSample>& harvest,
                                  const std::vector<ErsSample>& ers,
                                  const std::vector<InputSample>& inputs,
                                  const ErsPeaks& carPeaks);

// The lap distances where a lap starts braking: the brake goes past
// kPedalPressed after at least kBrakeOffBeforeS off it, and the application
// lasts at least kBrakeHoldS. Every gap off the brake shorter than that stays
// part of the same application, so ABS pulsing, pressure dips and trail-brake
// pulses neither split, shorten nor drop a zone. `corners` are the
// recording's track corners, when known.
std::vector<BrakingPoint> findBrakingPoints(const AnalysisLapProgress& lap,
                                            const std::vector<InputSample>& samples,
                                            const TrackCorners* corners = nullptr);

// Each lap's braking zones, in lap order. With a comparison lap, braking
// points pair one to one: nearest within kBrakingPairM, then zones that
// overlap; a zone only one lap brakes for pairs with the other lap's lift.
// Each lap's corners come from its own recording's track corners, when known.
BrakingPointsMap calculateBrakingPoints(const AnalysisLapProgress& current,
                                        const std::vector<InputSample>& currentInputs,
                                        const AnalysisLapProgress* comparison,
                                        const std::vector<InputSample>* comparisonInputs,
                                        const TrackCorners* currentCorners = nullptr,
                                        const TrackCorners* comparisonCorners = nullptr);

DriverInputsMap calculateDriverInputs(const AnalysisLapProgress& lap,
                                      const std::vector<InputSample>& samples);

SpeedMap calculateSpeed(const AnalysisLapProgress& lap,
                        const std::vector<InputSample>& samples,
                        const TrackCorners* corners = nullptr);

ErsUsageMap calculateErsUsage(const AnalysisLapProgress& lap,
                              const std::vector<ErsSample>& samples,
                              const std::vector<TrackStatusChange>& timeline,
                              const ErsPeaks& carPeaks);

// Runs `read` on an open recording under whatever lock guards it, returning
// false when no recording is open. Each call holds the lock only for its own
// read, so a whole-recording peak scan never stalls playback for long.
using RecordingAccess = std::function<bool(const std::function<void(const TnrdReader&)>& read)>;

struct AnalysisMapLap {
    RecordingAccess recording;
    int lapNum{};
    int driverIndex{-1};
};

// The complete map for `mode`: "dominance", "ers", "harvest", "inputs",
// "speed" or "braking". `comparison`
// may be null; Track Dominance then has nothing to compare and returns no
// sectors.
AnalysisMapResult buildAnalysisMap(const std::string& mode, const AnalysisMapLap& current,
                                   const AnalysisMapLap* comparison);

} // namespace tnrp
