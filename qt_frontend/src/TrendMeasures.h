#pragma once

#include <QHash>
#include <QVector>

#include <cstdint>
#include <limits>

#include "SessionModel.h"

// Per-lap ERS and fuel for the Trends page: a port of Electron's
// lib/trendMeasures.ts, run over the streamed driver's status history
// against its lap boundaries. The same code serves live and playback.

// Session time at which a lap began, as the chart axes label it.
struct TrendLapBoundary {
    int lapNum = 0;
    double sessionTime = 0;
};

// Session time at which `lapNum` began, using the latest attempt of a reused
// lap number; NaN when unknown.
double trendLapStart(const QVector<TrendLapBoundary>& boundaries, int lapNum);

// One completed lap's measurements; NaN where the lap was not measured.
struct TrendLapMeasure {
    double deployedMj = std::numeric_limits<double>::quiet_NaN();
    double harvestedMj = std::numeric_limits<double>::quiet_NaN();
    double fuelKg = std::numeric_limits<double>::quiet_NaN();
};

// Incremental scan of the status history. The newest row can still be
// patched in place by a later V6 sample at the same time, so it is never
// consumed.
class StintStatusScan {
public:
    struct Reset { double time = 0; double total = 0; };
    // The ERS counters are per-lap totals the game resets at the timing
    // line, so the last value before a reset is that lap's total.
    struct LapCounter {
        QVector<Reset> resets;
        // NaN while the value is unavailable, so a gap is never read as a reset.
        double latest = std::numeric_limits<double>::quiet_NaN();
        void push(double time, double value);
    };

    LapCounter deployed;
    // The two harvest counters are separate per-lap totals; each resets on its own.
    LapCounter harvestedK;
    LapCounter harvestedH;
    QVector<double> fuelRises;

    // False when the history was replaced or trimmed; start a new scan.
    bool update(const QVector<StsSample>& rows);

private:
    double lastFuel_ = std::numeric_limits<double>::quiet_NaN();
    qsizetype scanned_ = 0;
    double firstTime_ = std::numeric_limits<double>::quiet_NaN();
    double lastScannedTime_ = std::numeric_limits<double>::quiet_NaN();
};

// One scan kept across refreshes: extends it as the history grows, and starts
// again when held rows were replaced or rewritten (a new `revision`).
class StintStatusScanner {
public:
    const StintStatusScan& scanFor(const QVector<StsSample>& rows, quint64 revision);

private:
    bool started_ = false;
    quint64 revision_ = 0;
    StintStatusScan scan_;
};

// ERS used, ERS recharged and fuel used on each completed lap. Recharge is
// MGU-K plus MGU-H, or MGU-K alone under regulations without an MGU-H (the
// 2026 packet still carries a legacy MGU-H field).
QHash<int, TrendLapMeasure> measureTrendLaps(const StintStatusScan& scan,
                                             const QVector<StsSample>& rows,
                                             const QVector<TrendLapBoundary>& boundaries,
                                             const QVector<int>& laps, bool hasMguh);
