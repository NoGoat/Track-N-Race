#pragma once

// Per-graph view mode (Chart vs Table). Every telemetry graph can be swapped for
// a table of the raw sample values behind it. Mirrors CompactSettings.h: the enum
// + QSettings-key mapping live here so the pages (which read their own state at
// construction), MainWindow (which persists changes) and the Settings dialog all
// agree on the keys.
//
// Default for every graph is Chart (false). Table is true.

namespace tnr {

enum class GraphSection {
    OverviewTelemetry,      // Overview Speed / RPM / ERS chart

    // Tyre graphs — shared between the Tyres page and the Overview tyre strip
    // (both are TyreChartsWidget instances) so a choice applies consistently.
    TyreSurface,
    TyreInner,
    TyreBrake,
    TyreWear,

    InputGear,
    InputThrottleBrake,
    InputSteering,

    PowerSplit,
    PowerHarvest,
    PowerStore,
    PowerFuel,

    MiscGForce,
    MiscRideHeight,

    Count_
};

inline const char* graphViewKey(GraphSection s) {
    switch (s) {
        case GraphSection::OverviewTelemetry:  return "ui/graphView/overviewTelemetry";
        case GraphSection::TyreSurface:        return "ui/graphView/tyreSurface";
        case GraphSection::TyreInner:          return "ui/graphView/tyreInner";
        case GraphSection::TyreBrake:          return "ui/graphView/tyreBrake";
        case GraphSection::TyreWear:           return "ui/graphView/tyreWear";
        case GraphSection::InputGear:          return "ui/graphView/inputGear";
        case GraphSection::InputThrottleBrake: return "ui/graphView/inputThrottleBrake";
        case GraphSection::InputSteering:      return "ui/graphView/inputSteering";
        case GraphSection::PowerSplit:         return "ui/graphView/powerSplit";
        case GraphSection::PowerHarvest:       return "ui/graphView/powerHarvest";
        case GraphSection::PowerStore:         return "ui/graphView/powerStore";
        case GraphSection::PowerFuel:          return "ui/graphView/powerFuel";
        case GraphSection::MiscGForce:         return "ui/graphView/miscGForce";
        case GraphSection::MiscRideHeight:     return "ui/graphView/miscRideHeight";
        default:                               return "";
    }
}

}  // namespace tnr
