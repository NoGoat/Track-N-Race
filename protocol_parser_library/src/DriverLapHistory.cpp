#include "tnrp/DriverLapHistory.h"

#include <utility>

namespace tnrp {

DriverLapHistoryRow buildDriverLapHistory(int carIdx, int carCount, const CarLapsSource& lapsFor) {
    DriverLapHistoryRow out;
    out.car_idx = carIdx;
    // Every car's laps, for the session's fastest valid sectors.
    for (int car = 0; car < carCount; ++car) {
        auto laps = lapsFor(car, car == carIdx ? &out.stint_start_lap : nullptr);
        for (const auto& lap : laps) {
            const int ms[3] = {lap.s1_ms, lap.s2_ms, lap.s3_ms};
            const bool valid[3] = {lap.s1_valid, lap.s2_valid, lap.s3_valid};
            int* best[3] = {&out.overall_best_s1_ms, &out.overall_best_s2_ms,
                            &out.overall_best_s3_ms};
            for (int s = 0; s < 3; ++s)
                if (ms[s] > 0 && valid[s] && (!*best[s] || ms[s] < *best[s]))
                    *best[s] = ms[s];
        }
        if (car == carIdx) out.laps = std::move(laps);
    }
    int bestMs = 0;
    for (const auto& lap : out.laps) {
        if (!lap.lap_valid || lap.lap_time_ms <= 0) continue;
        if (!bestMs || lap.lap_time_ms < bestMs) {
            bestMs = lap.lap_time_ms;
            out.best_lap_num = lap.lap_num;
        }
    }
    return out;
}

} // namespace tnrp
