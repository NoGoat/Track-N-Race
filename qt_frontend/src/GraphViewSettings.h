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

    // Overview tyre strip graphs (its own TyreChartsWidget) — chosen independently
    // of the Tyres page's tyre graphs below.
    OverviewTyreSurface,
    OverviewTyreInner,
    OverviewTyreBrake,
    OverviewTyreWear,

    // Overview per-corner tyre cards — Card or Time/Surface/Inner/Brake table,
    // chosen independently of the Tyres page cards below.
    OverviewTyreCardFL,
    OverviewTyreCardFR,
    OverviewTyreCardRL,
    OverviewTyreCardRR,

    // Tyres page tyre graphs.
    TyreSurface,
    TyreInner,
    TyreBrake,
    TyreWear,

    // Tyres page per-corner tyre cards — each can show as its card or as a
    // Time/Surface/Inner/Brake table (mirrors Electron's tyreCardFL/FR/RL/RR).
    TyreCardFL,
    TyreCardFR,
    TyreCardRL,
    TyreCardRR,

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
        case GraphSection::OverviewTyreSurface: return "ui/graphView/overviewTyreSurface";
        case GraphSection::OverviewTyreInner:  return "ui/graphView/overviewTyreInner";
        case GraphSection::OverviewTyreBrake:  return "ui/graphView/overviewTyreBrake";
        case GraphSection::OverviewTyreWear:   return "ui/graphView/overviewTyreWear";
        case GraphSection::OverviewTyreCardFL: return "ui/graphView/overviewTyreCardFL";
        case GraphSection::OverviewTyreCardFR: return "ui/graphView/overviewTyreCardFR";
        case GraphSection::OverviewTyreCardRL: return "ui/graphView/overviewTyreCardRL";
        case GraphSection::OverviewTyreCardRR: return "ui/graphView/overviewTyreCardRR";
        case GraphSection::TyreSurface:        return "ui/graphView/tyreSurface";
        case GraphSection::TyreInner:          return "ui/graphView/tyreInner";
        case GraphSection::TyreBrake:          return "ui/graphView/tyreBrake";
        case GraphSection::TyreWear:           return "ui/graphView/tyreWear";
        case GraphSection::TyreCardFL:         return "ui/graphView/tyreCardFL";
        case GraphSection::TyreCardFR:         return "ui/graphView/tyreCardFR";
        case GraphSection::TyreCardRL:         return "ui/graphView/tyreCardRL";
        case GraphSection::TyreCardRR:         return "ui/graphView/tyreCardRR";
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
