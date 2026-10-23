#pragma once

#include <QHash>
#include <QVector>

#include <limits>

#include <tnrp/LapMeasures.h>

// Lap boundaries and per-lap ERS and fuel for the Trends page. The engine
// measures each lap (tnrp/LapMeasures.h) and pushes the streamed driver's
// laps as a lap_measures row; this only looks them up by lap.

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

QHash<int, TrendLapMeasure> trendLapMeasures(const tnrp::LapMeasuresRow& row);
