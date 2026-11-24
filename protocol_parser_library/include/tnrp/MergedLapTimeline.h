#pragma once
#include <optional>
#include <string>
#include <vector>

// Two recordings played on one timeline (the second-file comparison).
//
// Laps stay in step: lap N of file A plays with lap N of file B, each file
// counting its own driver's laps. A merged lap lasts as long as the slower
// file's lap; the quicker file finishes its lap and holds its last frame until
// the merged lap ends. A lap only one file has uses that file's length (the
// other shows "no lap N"). After the shorter file's last lap (a shorter race
// or a retirement) the timeline continues with the longer file alone.
//
// Every frontend builds the same table here and then only does arithmetic on
// it: a merged time falls in one segment, and each file's position is its span
// start plus the offset into the segment, held at the span's end.
namespace tnrp {

// One lap of one file's driver, in that file's session time.
struct TimelineLap {
    int lap_num{};
    float start{};
    float end{};
};

// One file's input: its laps, and how far its recording reaches before the
// first lap and after the last (absent when unknown).
struct TimelineFile {
    std::vector<TimelineLap> laps;
    std::optional<float> recording_start;
    std::optional<float> recording_end;
};

// One file's part of a merged segment, in that file's session time.
struct MergedSpan {
    float start{};
    float end{};
};

// One stretch of the merged timeline. kind: "pre" (before lap 1), "lap", or
// "post" (after the last lap). A missing span means that file has no part in
// it (no such lap, or its race is over) and holds.
struct MergedSegment {
    std::string kind;
    int lap_num{};
    double start{};     // merged seconds from the timeline's origin
    double duration{};
    std::optional<MergedSpan> a;
    std::optional<MergedSpan> b;
};

struct MergedLapTimelineRow {
    std::string type{"merged_lap_timeline"};
    double total{};
    // Each file's last lap number (0 = none), for "Finished L58" style labels.
    int a_last_lap{};
    int b_last_lap{};
    std::vector<MergedSegment> segments;
};

MergedLapTimelineRow buildMergedLapTimeline(const TimelineFile& a, const TimelineFile& b);

// One file's position at a merged time: inside its span it plays; after its
// span (or in a segment it has no part in) it holds the last frame it showed.
struct MergedFilePosition {
    std::optional<float> time;  // absent before the file's first frame
    bool holding{true};
};

struct MergedPosition {
    double time{};          // clamped to [0, total]
    size_t segment{};
    double offset{};        // seconds into the segment
    MergedFilePosition a;
    MergedFilePosition b;
};

// Where both files are at merged time `time` (the timeline must have segments).
MergedPosition mergedPositionAt(const MergedLapTimelineRow& timeline, double time);

// The merged time while file A plays (does not hold) at its session time
// `aTime`; absent when no segment of A covers it.
std::optional<double> mergedTimeForA(const MergedLapTimelineRow& timeline, float aTime);

// The timeline for an input in mergedLapTimelineJson's form; absent on
// malformed input or when the two files share no timeline.
std::optional<MergedLapTimelineRow> mergedLapTimelineFromInput(const std::string& inputJson);

// JSON in ({"a":TimelineFile,"b":TimelineFile}), JSON out (MergedLapTimelineRow):
// the form hosts without the C++ types call. Empty on malformed input.
std::string mergedLapTimelineJson(const std::string& inputJson);

} // namespace tnrp
