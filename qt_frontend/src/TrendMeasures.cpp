#include "TrendMeasures.h"

#include <algorithm>
#include <cmath>
#include <optional>

namespace {
// Status and timing packets can arrive either side of the line; a reset far
// from any line (a flashback or a garage visit) is not a lap total.
constexpr double kResetLineToleranceS = 5;
// A fuel sample further than this from a lap line cannot stand for the line.
constexpr double kFuelLineToleranceS = 2;
// Fuel only falls on track; any rise is a refuel or a reset.
constexpr double kFuelRiseKg = 0.01;

// The lap that ended at the line nearest `time`, or none when no line is close.
std::optional<int> lapEndingNear(const QVector<TrendLapBoundary>& boundaries, double time) {
    const auto it = std::lower_bound(boundaries.cbegin(), boundaries.cend(), time,
        [](const TrendLapBoundary& boundary, double value) { return boundary.sessionTime < value; });
    const qsizetype lo = qsizetype(std::distance(boundaries.cbegin(), it));
    qsizetype nearest = -1;
    for (qsizetype index : {lo - 1, lo}) {
        if (index < 0 || index >= boundaries.size()) continue;
        if (nearest == -1 || std::abs(boundaries[index].sessionTime - time) <
                             std::abs(boundaries[nearest].sessionTime - time))
            nearest = index;
    }
    if (nearest == -1 || std::abs(boundaries[nearest].sessionTime - time) > kResetLineToleranceS)
        return std::nullopt;
    return nearest > 0 ? boundaries[nearest - 1].lapNum : boundaries[nearest].lapNum - 1;
}

QHash<int, double> totalsByLap(const QVector<StintStatusScan::Reset>& resets,
                               const QVector<TrendLapBoundary>& boundaries) {
    QHash<int, double> totals;
    for (const StintStatusScan::Reset& reset : resets) {
        if (const std::optional<int> lap = lapEndingNear(boundaries, reset.time))
            totals.insert(*lap, reset.total / 1'000'000.0);
    }
    return totals;
}

double fuelAt(const QVector<StsSample>& rows, double time) {
    const auto it = std::lower_bound(rows.cbegin(), rows.cend(), time,
        [](const StsSample& sample, double value) { return sample.t < value; });
    const qsizetype after = qsizetype(std::distance(rows.cbegin(), it));
    double best = std::numeric_limits<double>::quiet_NaN(), bestGap = kFuelLineToleranceS;
    for (qsizetype i : {after - 1, after}) {
        if (i < 0 || i >= rows.size()) continue;
        const double gap = std::abs(rows[i].t - time);
        const double fuel = rows[i].fuel_kg;
        if (gap <= bestGap && std::isfinite(fuel)) { best = fuel; bestGap = gap; }
    }
    return best;
}
} // namespace

double trendLapStart(const QVector<TrendLapBoundary>& boundaries, int lapNum) {
    for (qsizetype i = boundaries.size() - 1; i >= 0; --i)
        if (boundaries[i].lapNum == lapNum) return boundaries[i].sessionTime;
    return std::numeric_limits<double>::quiet_NaN();
}

void StintStatusScan::LapCounter::push(double time, double value) {
    if (value < latest) resets.push_back({time, latest});
    latest = value;
}

bool StintStatusScan::update(const QVector<StsSample>& rows) {
    const qsizetype end = rows.size() - 1;
    if (end < scanned_ || (scanned_ > 0 &&
            (rows[0].t != firstTime_ || rows[scanned_ - 1].t != lastScannedTime_))) return false;
    for (qsizetype i = scanned_; i < end; ++i) {
        const StsSample& row = rows[i];
        deployed.push(row.t, row.ers_deployed_j);
        harvestedK.push(row.t, row.mguk_harvest_j);
        harvestedH.push(row.t, row.mguh_harvest_j);
        const double fuel = row.fuel_kg;
        if (fuel > lastFuel_ + kFuelRiseKg) fuelRises.push_back(row.t);
        lastFuel_ = fuel;
    }
    if (end > scanned_) {
        scanned_ = end;
        firstTime_ = rows[0].t;
        lastScannedTime_ = rows[end - 1].t;
    }
    return true;
}

const StintStatusScan& StintStatusScanner::scanFor(const QVector<StsSample>& rows, quint64 revision) {
    if (!started_ || revision_ != revision || !scan_.update(rows)) {
        started_ = true;
        revision_ = revision;
        scan_ = StintStatusScan{};
        scan_.update(rows);
    }
    return scan_;
}

QHash<int, TrendLapMeasure> measureTrendLaps(const StintStatusScan& scan,
                                             const QVector<StsSample>& rows,
                                             const QVector<TrendLapBoundary>& boundaries,
                                             const QVector<int>& laps, bool hasMguh) {
    const QHash<int, double> deployed = totalsByLap(scan.deployed.resets, boundaries);
    const QHash<int, double> harvestedK = totalsByLap(scan.harvestedK.resets, boundaries);
    const QHash<int, double> harvestedH = hasMguh ? totalsByLap(scan.harvestedH.resets, boundaries)
                                                  : QHash<int, double>{};
    QHash<int, TrendLapMeasure> out;
    for (int lap : laps) {
        const double start = trendLapStart(boundaries, lap);
        const double end = trendLapStart(boundaries, lap + 1);
        TrendLapMeasure measure;
        if (std::isfinite(start) && std::isfinite(end) &&
            std::none_of(scan.fuelRises.cbegin(), scan.fuelRises.cend(),
                         [&](double time) { return time > start && time <= end; })) {
            const double used = fuelAt(rows, start) - fuelAt(rows, end);
            if (used >= 0) measure.fuelKg = used;
        }
        if (deployed.contains(lap)) measure.deployedMj = deployed.value(lap);
        const bool haveK = harvestedK.contains(lap);
        const bool haveH = !hasMguh || harvestedH.contains(lap);
        if (haveK && haveH)
            measure.harvestedMj = harvestedK.value(lap) + (hasMguh ? harvestedH.value(lap) : 0.0);
        out.insert(lap, measure);
    }
    return out;
}
