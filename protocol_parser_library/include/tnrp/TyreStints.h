#pragma once

#include <vector>

#include "tnrp/control_rows.h"

namespace tnrp {

// Session History reports each tyre stint by its end lap only, with 255 for
// the stint still in use. The spec says only "Lap the tyre usage ends on".
// Recordings show the tyres change on lap end_lap + 1 (the in-lap, still on
// the outgoing set), so the next stint starts on end_lap + 2.
inline constexpr int kOpenTyreStintEndLap = 255;

struct TyreStintRange {
    int start_lap{1};
    // Last lap on this set, including the in-lap; kOpenTyreStintEndLap if in use.
    int end_lap{kOpenTyreStintEndLap};
    int actual_compound{};
    int visual_compound{};
    bool contains(int lap) const {
        return lap >= start_lap && (end_lap == kOpenTyreStintEndLap || lap <= end_lap);
    }
};

// `project` maps a source stint to its Session History form, so recorded V6
// summaries and live Session History share this one definition.
template <class Stint, class Project>
std::vector<TyreStintRange> tyreStintRanges(const std::vector<Stint>& stints, Project project) {
    std::vector<TyreStintRange> out;
    int start = 1;
    for (const auto& source : stints) {
        const SessionHistoryTyreStint stint = project(source);
        const bool open = stint.end_lap == kOpenTyreStintEndLap;
        const int inLap = open ? kOpenTyreStintEndLap : stint.end_lap + 1;
        out.push_back({start, inLap, stint.actual_compound, stint.visual_compound});
        if (open) break;
        start = inLap + 1;
    }
    return out;
}

inline std::vector<TyreStintRange> tyreStintRanges(const std::vector<SessionHistoryTyreStint>& stints) {
    return tyreStintRanges(stints, [](const SessionHistoryTyreStint& stint) { return stint; });
}

inline const TyreStintRange* tyreStintForLap(const std::vector<TyreStintRange>& ranges, int lap) {
    for (const auto& range : ranges)
        if (range.contains(lap)) return &range;
    return nullptr;
}

// First lap of the stint in use on `lap`: the latest stint starting at or
// before it, which also covers a finished race past its last closed stint.
// 0 when no stint is known.
inline int tyreStintStartLap(const std::vector<TyreStintRange>& ranges, int lap) {
    int start = 0;
    for (const auto& range : ranges)
        if (range.start_lap <= lap) start = range.start_lap;
    return start;
}

} // namespace tnrp
