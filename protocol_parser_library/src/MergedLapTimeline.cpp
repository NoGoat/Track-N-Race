#include "tnrp/MergedLapTimeline.h"

#include <algorithm>
#include <limits>
#include <map>

#include <glaze/glaze.hpp>

namespace tnrp {
namespace {

// A file's laps by number; a reused lap number keeps its latest attempt.
std::map<int, TimelineLap> lapsByNumber(const TimelineFile& file) {
    std::map<int, TimelineLap> out;
    for (const auto& lap : file.laps)
        if (lap.lap_num >= 1 && lap.end > lap.start) out[lap.lap_num] = lap;
    return out;
}

double spanLength(const std::optional<MergedSpan>& span) {
    return span ? static_cast<double>(span->end - span->start) : 0.0;
}

} // namespace

MergedLapTimelineRow buildMergedLapTimeline(const TimelineFile& a, const TimelineFile& b) {
    MergedLapTimelineRow out;
    const auto aLaps = lapsByNumber(a);
    const auto bLaps = lapsByNumber(b);
    if (aLaps.empty() && bLaps.empty()) return out;
    out.a_last_lap = aLaps.empty() ? 0 : aLaps.rbegin()->first;
    out.b_last_lap = bLaps.empty() ? 0 : bLaps.rbegin()->first;

    double cursor = 0.0;
    const auto push = [&](std::string kind, int lapNum, std::optional<MergedSpan> spanA, std::optional<MergedSpan> spanB) {
        const double duration = std::max(spanLength(spanA), spanLength(spanB));
        if (duration <= 0.0) return;
        out.segments.push_back({std::move(kind), lapNum, cursor, duration, spanA, spanB});
        cursor += duration;
    };

    // Before lap 1: the grid and formation lap, as far as each recording reaches.
    const auto preSpan = [](const TimelineFile& file, const std::map<int, TimelineLap>& laps) -> std::optional<MergedSpan> {
        if (laps.empty()) return std::nullopt;
        const float firstStart = laps.begin()->second.start;
        if (!file.recording_start || *file.recording_start >= firstStart) return std::nullopt;
        return MergedSpan{*file.recording_start, firstStart};
    };
    push("pre", 0, preSpan(a, aLaps), preSpan(b, bLaps));

    // Every lap number either file has, in order; a number both lack is skipped.
    const int first = std::min(aLaps.empty() ? std::numeric_limits<int>::max() : aLaps.begin()->first,
                               bLaps.empty() ? std::numeric_limits<int>::max() : bLaps.begin()->first);
    const int last = std::max(out.a_last_lap, out.b_last_lap);
    for (int lap = first; lap <= last; ++lap) {
        const auto spanOf = [lap](const std::map<int, TimelineLap>& laps) -> std::optional<MergedSpan> {
            const auto it = laps.find(lap);
            if (it == laps.end()) return std::nullopt;
            return MergedSpan{it->second.start, it->second.end};
        };
        push("lap", lap, spanOf(aLaps), spanOf(bLaps));
    }

    // After the last lap: only a file whose race ran to the final lap number
    // continues; the other has already finished or retired and holds.
    const auto postSpan = [last](const TimelineFile& file, const std::map<int, TimelineLap>& laps) -> std::optional<MergedSpan> {
        if (laps.empty() || laps.rbegin()->first != last) return std::nullopt;
        const float lastEnd = laps.rbegin()->second.end;
        if (!file.recording_end || *file.recording_end <= lastEnd) return std::nullopt;
        return MergedSpan{lastEnd, *file.recording_end};
    };
    push("post", 0, postSpan(a, aLaps), postSpan(b, bLaps));

    out.total = cursor;
    return out;
}

namespace {
struct MergedLapTimelineInput {
    TimelineFile a;
    TimelineFile b;
};
} // namespace

std::optional<MergedLapTimelineRow> mergedLapTimelineFromInput(const std::string& inputJson) {
    MergedLapTimelineInput input;
    if (glz::read<glz::opts{.error_on_unknown_keys = false}>(input, inputJson)) return std::nullopt;
    MergedLapTimelineRow row = buildMergedLapTimeline(input.a, input.b);
    if (row.segments.empty()) return std::nullopt;
    return row;
}

namespace {
MergedFilePosition filePosition(const MergedLapTimelineRow& timeline, size_t index, double offset,
                                std::optional<MergedSpan> MergedSegment::*pick) {
    if (const auto& span = timeline.segments[index].*pick) {
        const double length = span->end - span->start;
        if (offset <= length) return {static_cast<float>(span->start + offset), false};
        return {span->end, true};
    }
    for (size_t i = index; i-- > 0;)
        if (const auto& earlier = timeline.segments[i].*pick) return {earlier->end, true};
    return {std::nullopt, true};
}
} // namespace

MergedPosition mergedPositionAt(const MergedLapTimelineRow& timeline, double time) {
    MergedPosition out;
    if (timeline.segments.empty()) return out;
    out.time = std::clamp(time, 0.0, timeline.total);
    // The last segment starting at or before the time.
    size_t lo = 0, hi = timeline.segments.size() - 1;
    while (lo < hi) {
        const size_t mid = (lo + hi + 1) / 2;
        if (timeline.segments[mid].start <= out.time) lo = mid;
        else hi = mid - 1;
    }
    out.segment = lo;
    out.offset = std::max(0.0, out.time - timeline.segments[lo].start);
    out.a = filePosition(timeline, lo, out.offset, &MergedSegment::a);
    out.b = filePosition(timeline, lo, out.offset, &MergedSegment::b);
    return out;
}

std::optional<double> mergedTimeForA(const MergedLapTimelineRow& timeline, float aTime) {
    const auto& segments = timeline.segments;
    std::optional<size_t> found;
    for (size_t i = 0; i < segments.size() && !found; ++i)
        if (segments[i].a && aTime >= segments[i].a->start && aTime < segments[i].a->end) found = i;
    // Exactly on (or past) the final line: the last segment A plays in.
    for (size_t i = segments.size(); i-- > 0 && !found;)
        if (segments[i].a && aTime >= segments[i].a->end) found = i;
    if (!found) return std::nullopt;
    const auto& segment = segments[*found];
    const double length = segment.a->end - segment.a->start;
    return segment.start + std::clamp(static_cast<double>(aTime - segment.a->start), 0.0, length);
}

std::string mergedLapTimelineJson(const std::string& inputJson) {
    MergedLapTimelineInput input;
    if (glz::read<glz::opts{.error_on_unknown_keys = false}>(input, inputJson)) return {};
    std::string out;
    (void)glz::write_json(buildMergedLapTimeline(input.a, input.b), out);
    return out;
}

} // namespace tnrp
