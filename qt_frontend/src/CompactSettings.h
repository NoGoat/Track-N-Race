#pragma once

#include <QMetaType>
#include <QString>
#include <QVariant>

// Per-section density settings. Ordinary sections use Compact/Normal/Spacious.
// The three specialised controls
// keep their integer sub-levels, including their dedicated Spacious value.
//
// The enum + key mapping live here so the pages (which read their own state at
// construction), MainWindow (which persists changes) and the Settings dialog all
// agree on the QSettings keys.

namespace tnr {

enum class DensityMode { Compact = 0, Normal = 1, Spacious = 2 };

enum class CompactSection {
    OverviewStats,     // Overview stats row
    OverviewDamage,    // Overview damage cards
    OverviewTyres,     // Overview tyre cards
    StandingsTable,
    StandingsTiming,
    StandingsErs,
    StandingsStrategy,
    SessionCards,      // Session info/stat cards
    SessionProximity,  // Session proximity widget (Normal vs Compact)
    SessionEvents,     // Session events log (Normal vs Compact)
    SessionWeather,    // 4 Spacious, 0 Normal, 1..3 Compact
    SessionHeader,     // 3 Spacious, 0 Normal, 1..2 Compact
    PowerCards,        // Power info cards
    StrategySummary,   // Strategy summary header (lap / tyre bar / cliff)
    PlaybackBar,
    Count_
};

inline const char* compactKey(CompactSection s) {
    switch (s) {
        case CompactSection::OverviewStats:   return "ui/compact/overviewStats";
        case CompactSection::OverviewDamage:  return "ui/compact/overviewDamage";
        case CompactSection::OverviewTyres:   return "ui/compact/overviewTyres";
        case CompactSection::StandingsTable:  return "ui/compact/standingsTable";
        case CompactSection::StandingsTiming: return "ui/compact/standingsTiming";
        case CompactSection::StandingsErs:    return "ui/compact/standingsErs";
        case CompactSection::StandingsStrategy: return "ui/compact/standingsStrategy";
        case CompactSection::SessionCards:    return "ui/compact/sessionCards";
        case CompactSection::SessionProximity: return "ui/compact/sessionProximity";
        case CompactSection::SessionEvents:   return "ui/compact/sessionEvents";
        case CompactSection::SessionWeather:  return "ui/compact/sessionWeather";
        case CompactSection::SessionHeader:   return "ui/compact/sessionHeader";
        case CompactSection::PowerCards:      return "ui/compact/powerCards";
        case CompactSection::StrategySummary: return "ui/compact/strategySummary";
        case CompactSection::PlaybackBar:     return "ui/compact/playbackBar";
        default:                              return "";
    }
}

inline const char* densityValue(DensityMode mode) {
    return mode == DensityMode::Compact ? "compact"
        : mode == DensityMode::Spacious ? "spacious" : "normal";
}

inline DensityMode densityFromValue(const QVariant& value) {
    if (value.metaType().id() == QMetaType::Bool)
        return value.toBool() ? DensityMode::Compact : DensityMode::Normal;
    const QString text = value.toString();
    if (text == "compact" || text == "true" || text == "1") return DensityMode::Compact;
    if (text == "spacious") return DensityMode::Spacious;
    return DensityMode::Normal;
}

}  // namespace tnr
