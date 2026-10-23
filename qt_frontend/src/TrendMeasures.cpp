#include "TrendMeasures.h"

double trendLapStart(const QVector<TrendLapBoundary>& boundaries, int lapNum) {
    for (qsizetype i = boundaries.size() - 1; i >= 0; --i)
        if (boundaries[i].lapNum == lapNum) return boundaries[i].sessionTime;
    return std::numeric_limits<double>::quiet_NaN();
}

QHash<int, TrendLapMeasure> trendLapMeasures(const tnrp::LapMeasuresRow& row) {
    QHash<int, TrendLapMeasure> out;
    out.reserve(static_cast<qsizetype>(row.laps.size()));
    for (const tnrp::LapMeasure& lap : row.laps) {
        TrendLapMeasure measure;
        if (lap.ers_deployed_mj) measure.deployedMj = *lap.ers_deployed_mj;
        if (lap.ers_harvested_mj) measure.harvestedMj = *lap.ers_harvested_mj;
        if (lap.fuel_used_kg) measure.fuelKg = *lap.fuel_used_kg;
        out.insert(lap.lap_num, measure);
    }
    return out;
}
