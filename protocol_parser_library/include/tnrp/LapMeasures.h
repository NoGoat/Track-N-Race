#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

// ERS used, ERS recharged and fuel used on each completed lap of one car,
// shared by live telemetry and V6 playback so every frontend shows the same
// numbers.
//
// The game's ERS counters are per-lap totals that it zeroes at the timing
// line, so a lap's total is the counter's value just before that reset. The
// reset is looked for only around the lap's own line: the largest fall in the
// counter there is the reset, and anything else the counter does during the
// lap is ignored: a flashback's restored state can sit a few kJ below the last
// sample before its target, and a recording keeps that seam. Another car's
// lap timing arrives about twice a second, so its recorded line can trail the
// reset by up to half a second; the window covers that.
//
// Fuel used is the fuel mass at the lap's starting line less the mass at its
// finishing line. A flashback rewinds the measurement with the timeline, so
// fuel never rises within a measured lap; a negative difference is not a
// measurement.

namespace tnrp {

// How far either side of a lap line a sample can stand for the line.
inline constexpr float kLapLineWindowS = 2.0f;

// One status sample of the counters a lap measurement reads. NaN marks a field
// this sample does not carry (restricted, or not recorded at this time).
struct LapLineSample {
    float time{};
    double deployedJ{};
    double harvestKJ{};
    double harvestHJ{};
    double fuelKg{};
};

// The counters' readings at one lap line.
struct LapLineReading {
    std::optional<double> deployedJ;
    std::optional<double> harvestKJ;
    std::optional<double> harvestHJ;
    std::optional<double> fuelKg;
};

// Reads the line at `line` from time-ascending samples covering at least
// [line - kLapLineWindowS, line + kLapLineWindowS]; samples outside it are
// ignored.
LapLineReading readLapLine(std::span<const LapLineSample> samples, float line);

// One completed lap, before the regulations decide what counts as recharge.
struct LapTotals {
    float start{};
    float end{};
    std::optional<double> deployedJ;
    std::optional<double> harvestKJ;
    std::optional<double> harvestHJ;
    std::optional<double> fuelUsedKg;
};

// Lap `lap` from the readings at its two lines.
LapTotals lapTotals(float start, float end, const LapLineReading& atStart,
                    const LapLineReading& atEnd);

// ── lap_measures (pushed when it changes) ───────────────────────────────────
// The streamed driver's completed laps: the player live, the driver selector's
// car in playback, up to the cursor. Absent members are not measured.

struct LapMeasure {
    int lap_num{};
    std::optional<double> ers_deployed_mj;
    // MGU-K, plus MGU-H under regulations that have one.
    std::optional<double> ers_harvested_mj;
    std::optional<double> fuel_used_kg;
};

struct LapMeasuresRow {
    std::string type{"lap_measures"};
    int car_idx{-1};
    std::vector<LapMeasure> laps;
};

LapMeasure lapMeasure(int lapNum, const LapTotals& totals, bool hasMguh);

// The live side: one car's laps as its status and lap rows arrive. It holds
// only the samples a pending line still needs, a few seconds' worth.
class LiveLapMeasures {
public:
    void clear();
    // The player's status row. Another car's row starts over for that car.
    void addStatus(int car, const LapLineSample& sample);
    // The player's lap row: `lapNum` as of `time`, `currentLapMs` into it.
    void addLap(int car, float time, int lapNum, double currentLapMs);
    // The timeline went back to `time` (a flashback): laps and samples after it
    // are gone.
    void rewind(float time);

    int car() const { return car_; }
    // Bumped whenever the completed laps change.
    uint64_t revision() const { return revision_; }
    LapMeasuresRow row(bool hasMguh) const;

private:
    struct Line {
        float time{};
        int endingLap{};   // 0 when the line only starts a lap
        float endingStart{};
        int nextLap{};
    };
    void reset(int car);
    void settle(float now);

    int car_{-1};
    int lap_{};
    float lapStart_{};
    std::vector<LapLineSample> samples_;
    std::vector<Line> pending_;
    std::map<int, float> starts_;               // lap -> start time
    std::map<int, double> startFuel_;           // lap -> fuel at its start line
    std::map<int, LapTotals> laps_;
    uint64_t revision_{};
};

} // namespace tnrp
