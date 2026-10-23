#include "tnrp/LapMeasures.h"

#include <algorithm>
#include <cmath>

namespace tnrp {

namespace {
// Samples older than every line still to be read, by more than this, are dropped.
constexpr float kSampleSlackS = 0.5f;

// The total of the lap ending at `line` held by a per-lap counter: its value
// before the largest fall near the line. A counter that did not fall there was
// zero all lap (nothing used), so its last value before the line stands.
std::optional<double> counterAt(std::span<const LapLineSample> samples, float line,
                                double LapLineSample::*field) {
    std::optional<double> previous, total, beforeLine;
    double largestFall = 0.0;
    for (const LapLineSample& sample : samples) {
        if (sample.time < line - kLapLineWindowS) continue;
        if (sample.time > line + kLapLineWindowS) break;
        const double value = sample.*field;
        if (!std::isfinite(value)) continue;
        if (previous && *previous - value > largestFall) {
            largestFall = *previous - value;
            total = *previous;
        }
        if (sample.time < line) beforeLine = value;
        previous = value;
    }
    return total ? total : beforeLine;
}

// The fuel mass at `line`: the sample nearest it.
std::optional<double> fuelAt(std::span<const LapLineSample> samples, float line) {
    std::optional<double> best;
    float bestGap = kLapLineWindowS;
    for (const LapLineSample& sample : samples) {
        const float gap = std::abs(sample.time - line);
        if (sample.time > line + kLapLineWindowS) break;
        if (gap <= bestGap && std::isfinite(sample.fuelKg)) {
            best = sample.fuelKg;
            bestGap = gap;
        }
    }
    return best;
}
} // namespace

LapLineReading readLapLine(std::span<const LapLineSample> samples, float line) {
    return {counterAt(samples, line, &LapLineSample::deployedJ),
            counterAt(samples, line, &LapLineSample::harvestKJ),
            counterAt(samples, line, &LapLineSample::harvestHJ),
            fuelAt(samples, line)};
}

LapTotals lapTotals(float start, float end, const LapLineReading& atStart,
                    const LapLineReading& atEnd) {
    LapTotals totals{start, end, atEnd.deployedJ, atEnd.harvestKJ, atEnd.harvestHJ, std::nullopt};
    if (atStart.fuelKg && atEnd.fuelKg && *atStart.fuelKg >= *atEnd.fuelKg)
        totals.fuelUsedKg = *atStart.fuelKg - *atEnd.fuelKg;
    return totals;
}

LapMeasure lapMeasure(int lapNum, const LapTotals& totals, bool hasMguh) {
    LapMeasure out;
    out.lap_num = lapNum;
    if (totals.deployedJ) out.ers_deployed_mj = *totals.deployedJ / 1'000'000.0;
    if (totals.harvestKJ && (!hasMguh || totals.harvestHJ))
        out.ers_harvested_mj = (*totals.harvestKJ + (hasMguh ? *totals.harvestHJ : 0.0)) / 1'000'000.0;
    out.fuel_used_kg = totals.fuelUsedKg;
    return out;
}

void LiveLapMeasures::clear() { reset(-1); }

void LiveLapMeasures::reset(int car) {
    car_ = car;
    lap_ = 0;
    lapStart_ = 0.0f;
    samples_.clear();
    pending_.clear();
    starts_.clear();
    startFuel_.clear();
    laps_.clear();
    ++revision_;
}

void LiveLapMeasures::addStatus(int car, const LapLineSample& sample) {
    if (car < 0 || !std::isfinite(sample.time)) return;
    if (car != car_) reset(car);
    // A late packet from before the newest sample replaces what followed it.
    while (!samples_.empty() && samples_.back().time > sample.time) samples_.pop_back();
    samples_.push_back(sample);
    settle(sample.time);
}

void LiveLapMeasures::addLap(int car, float time, int lapNum, double currentLapMs) {
    if (car < 0 || lapNum <= 0 || !std::isfinite(time)) return;
    if (car != car_) reset(car);
    if (lapNum == lap_) return;
    if (lap_ != 0 && lapNum < lap_) reset(car);   // a restart the timeline did not rewind
    if (lap_ == 0) {
        // Joined mid-lap: the lap began current_lap_ms before this row.
        const float start = std::isfinite(currentLapMs) && currentLapMs > 0.0
            ? time - static_cast<float>(currentLapMs / 1000.0) : time;
        pending_.push_back({start, 0, 0.0f, lapNum});
        lapStart_ = start;
    } else {
        pending_.push_back({time, lap_, lapStart_, lapNum});
        lapStart_ = time;
    }
    lap_ = lapNum;
    starts_[lap_] = lapStart_;
}

void LiveLapMeasures::settle(float now) {
    size_t settled = 0;
    for (; settled < pending_.size() && now >= pending_[settled].time + kLapLineWindowS; ++settled) {
        const Line& line = pending_[settled];
        const LapLineReading reading = readLapLine(samples_, line.time);
        if (reading.fuelKg) startFuel_[line.nextLap] = *reading.fuelKg;
        if (line.endingLap <= 0) continue;
        LapLineReading atStart;
        if (const auto fuel = startFuel_.find(line.endingLap); fuel != startFuel_.end())
            atStart.fuelKg = fuel->second;
        laps_[line.endingLap] = lapTotals(line.endingStart, line.time, atStart, reading);
        ++revision_;
    }
    pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(settled));

    // Keep what the next line can need: the window before the oldest pending
    // line, or before now when the next line is still to come.
    const float oldest = pending_.empty() ? now : std::min(now, pending_.front().time);
    const float keepFrom = oldest - kLapLineWindowS - kSampleSlackS;
    if (!samples_.empty() && samples_.front().time < keepFrom - 1.0f) {
        const auto first = std::lower_bound(samples_.begin(), samples_.end(), keepFrom,
            [](const LapLineSample& sample, float time) { return sample.time < time; });
        samples_.erase(samples_.begin(), first);
    }
}

void LiveLapMeasures::rewind(float time) {
    if (car_ < 0) return;
    while (!samples_.empty() && samples_.back().time > time) samples_.pop_back();
    std::erase_if(pending_, [&](const Line& line) { return line.time > time; });
    const size_t before = laps_.size();
    std::erase_if(laps_, [&](const auto& lap) { return lap.second.end > time; });
    if (laps_.size() != before) ++revision_;
    std::erase_if(starts_, [&](const auto& start) { return start.second > time; });
    for (auto fuel = startFuel_.begin(); fuel != startFuel_.end();)
        fuel = starts_.contains(fuel->first) ? std::next(fuel) : startFuel_.erase(fuel);
    // The lap holding the target is the current lap again.
    lap_ = starts_.empty() ? 0 : starts_.rbegin()->first;
    lapStart_ = starts_.empty() ? 0.0f : starts_.rbegin()->second;
}

LapMeasuresRow LiveLapMeasures::row(bool hasMguh) const {
    LapMeasuresRow out;
    out.car_idx = car_;
    out.laps.reserve(laps_.size());
    for (const auto& [lapNum, totals] : laps_) out.laps.push_back(lapMeasure(lapNum, totals, hasMguh));
    return out;
}

} // namespace tnrp
