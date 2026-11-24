#pragma once

#include "tnrp/control_rows.h"

#include <functional>
#include <vector>

// One car's driver_lap_history row (the lap-times view) from every car's
// completed laps, shared by the engine (live and playback) and the second
// file's stream so both build it the same way.
namespace tnrp {

// A car's completed laps; sets *stintStartLap, when given, to the first lap of
// the tyre stint in use.
using CarLapsSource = std::function<std::vector<SessionHistoryLap>(int car, int* stintStartLap)>;

// Car `carIdx`'s laps and stint start, its best valid lap, and the fastest
// valid sectors across cars 0..carCount-1.
DriverLapHistoryRow buildDriverLapHistory(int carIdx, int carCount, const CarLapsSource& lapsFor);

} // namespace tnrp
