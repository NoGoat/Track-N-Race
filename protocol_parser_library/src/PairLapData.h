#pragma once

#include "tnrp/control_rows.h"

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace tnrp {

// A paired phone's Analysis page asks for one lap at a time, naming the
// "family.field" channels it draws (for example "telemetry.speed_kph" or
// "status.ers_pct"). A full playback_lap_data row carries every field of every
// sample, several MB for a lap, so the desktop sends only those columns.

// Row families (1 << row type) the channels need, plus lap progress and
// positions, which every Analysis lap carries for its distance axis and map.
uint32_t pairLapDataRowMask(const std::vector<std::string>& channels);

// Reduces a playback_lap_data row to the requested columns. V6 field patches
// are merged forward, so every emitted sample holds each field's value at that
// time; an explicit {"available":false} clears the fields its type carried.
// Times are seconds from the lap's start. Returns "" when the row is unusable.
std::string pairLapDataJson(std::string_view playbackLapData,
                            const std::vector<std::string>& channels);

// The same reply built from samples read straight from a recording's columns,
// with no playback_lap_data row in between. Each family's samples are in row
// order, their times and values as that row's JSON would read them. Positions
// are left empty: only a chart lap request, which draws no map, takes this path.
struct PairLapSample {
    double sessionTime{};
    int v6Type{};
    bool unavailable{};
    std::vector<std::pair<std::string_view, double>> fields;
};
struct PairLapSamples {
    int lapNum{};
    float startSessionTime{};
    float endSessionTime{};
    std::vector<LapProgressPoint> progress;
    // By family name: "telemetry", "status", "damage", "motion", "motion_ex".
    std::map<std::string, std::vector<PairLapSample>, std::less<>> families;
};
std::string pairLapDataJson(const PairLapSamples& lap, const std::vector<std::string>& channels);

} // namespace tnrp
