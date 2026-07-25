#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace tnr::chart {

struct Point {
    double x{};
    double y{};
};

struct Segment {
    float x0{};
    float y0{};
    float x1{};
    float y1{};
};

struct ReducedSegments {
    std::vector<Segment> segments;
    std::size_t visiblePointCount{};
    bool reduced{};
};

// Selects the visible monotonic range, including one point on each side so a
// line crosses the plot boundary cleanly. At high density each pixel bucket
// retains first/min/max/last in source order, preserving spikes and gaps.
ReducedSegments buildSegments(
    std::span<const Point> points,
    double xMinimum,
    double xMaximum,
    std::uint32_t physicalWidth);

} // namespace tnr::chart
