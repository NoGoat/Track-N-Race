#pragma once

// Per-section compact-density settings. The single global "compact mode" was split
// into one setting per compact-able section so the user can pick and choose. Each
// Most sections are Normal (false) or Compact (true). Tyre cards and the weather
// strip use integer density levels, which is why Settings uses segmented controls
// rather than checkboxes.
//
// The enum + key mapping live here so the pages (which read their own state at
// construction), MainWindow (which persists changes) and the Settings dialog all
// agree on the QSettings keys.

namespace tnr {

enum class CompactSection {
    OverviewStats,     // Overview stats row
    OverviewDamage,    // Overview damage cards
    OverviewTyres,     // Overview tyre cards
    SessionCards,      // Session info/stat cards
    SessionProximity,  // Session proximity widget (Normal vs Compact)
    SessionEvents,     // Session events log (Normal vs Compact)
    SessionWeather,    // Session weather strip (0 Normal, 1 Compact 1, 2 Compact 2, 3 Compact 3)
    SessionHeader,     // Session header (0 Normal, 1 Compact 1, 2 Compact 2)
    PowerCards,        // Power info cards
    StrategySummary,   // Strategy summary header (lap / tyre bar / cliff)
    Count_
};

inline const char* compactKey(CompactSection s) {
    switch (s) {
        case CompactSection::OverviewStats:   return "ui/compact/overviewStats";
        case CompactSection::OverviewDamage:  return "ui/compact/overviewDamage";
        case CompactSection::OverviewTyres:   return "ui/compact/overviewTyres";
        case CompactSection::SessionCards:    return "ui/compact/sessionCards";
        case CompactSection::SessionProximity: return "ui/compact/sessionProximity";
        case CompactSection::SessionEvents:   return "ui/compact/sessionEvents";
        case CompactSection::SessionWeather:  return "ui/compact/sessionWeather";
        case CompactSection::SessionHeader:   return "ui/compact/sessionHeader";
        case CompactSection::PowerCards:      return "ui/compact/powerCards";
        case CompactSection::StrategySummary: return "ui/compact/strategySummary";
        default:                              return "";
    }
}

}  // namespace tnr
