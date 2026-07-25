#include "ChartData.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace tnr::chart {
namespace {

bool finite(const Point& point) {
    return std::isfinite(point.x) && std::isfinite(point.y);
}

void appendSegment(
    std::vector<Segment>& output,
    const Point& first,
    const Point& second,
    const double xOrigin) {
    if (!finite(first) || !finite(second)) {
        return;
    }
    output.push_back({
        static_cast<float>(first.x - xOrigin),
        static_cast<float>(first.y),
        static_cast<float>(second.x - xOrigin),
        static_cast<float>(second.y),
    });
}

void appendBucket(
    std::vector<std::size_t>& selected,
    std::size_t first,
    std::size_t last,
    std::size_t minimum,
    std::size_t maximum) {
    std::size_t candidates[] = {first, minimum, maximum, last};
    std::sort(std::begin(candidates), std::end(candidates));
    for (const auto index : candidates) {
        if (selected.empty() || selected.back() != index) {
            selected.push_back(index);
        }
    }
}

} // namespace

ReducedSegments buildSegments(
    const std::span<const Point> points,
    const double xMinimum,
    const double xMaximum,
    const std::uint32_t physicalWidth) {
    ReducedSegments result;
    if (points.size() < 2 || !std::isfinite(xMinimum) ||
        !std::isfinite(xMaximum) || xMaximum <= xMinimum ||
        physicalWidth == 0) {
        return result;
    }

    const auto lower = std::lower_bound(
        points.begin(), points.end(), xMinimum,
        [](const Point& point, const double value) { return point.x < value; });
    const auto upper = std::upper_bound(
        points.begin(), points.end(), xMaximum,
        [](const double value, const Point& point) { return value < point.x; });

    auto first = static_cast<std::size_t>(lower - points.begin());
    auto last = static_cast<std::size_t>(upper - points.begin());
    if (first > 0) {
        --first;
    }
    if (last < points.size()) {
        ++last;
    }
    if (last <= first + 1) {
        return result;
    }

    result.visiblePointCount = last - first;
    result.reduced =
        result.visiblePointCount > static_cast<std::size_t>(physicalWidth) * 2;

    if (!result.reduced) {
        result.segments.reserve(result.visiblePointCount - 1);
        for (auto index = first + 1; index < last; ++index) {
            appendSegment(
                result.segments, points[index - 1], points[index], xMinimum);
        }
        return result;
    }

    const auto span = xMaximum - xMinimum;
    std::vector<std::size_t> selected;
    selected.reserve(static_cast<std::size_t>(physicalWidth) * 4 + 8);

    std::size_t runStart = first;
    while (runStart < last) {
        while (runStart < last && !finite(points[runStart])) {
            ++runStart;
        }
        if (runStart >= last) {
            break;
        }
        auto runEnd = runStart + 1;
        while (runEnd < last && finite(points[runEnd])) {
            ++runEnd;
        }

        selected.clear();
        auto bucketStart = runStart;
        while (bucketStart < runEnd) {
            const auto normalized =
                (points[bucketStart].x - xMinimum) / span;
            const auto bucket = std::clamp(
                static_cast<long long>(
                    std::floor(normalized * physicalWidth)),
                0LL,
                static_cast<long long>(physicalWidth - 1));

            auto bucketEnd = bucketStart + 1;
            auto minimum = bucketStart;
            auto maximum = bucketStart;
            while (bucketEnd < runEnd) {
                const auto nextNormalized =
                    (points[bucketEnd].x - xMinimum) / span;
                const auto nextBucket = std::clamp(
                    static_cast<long long>(
                        std::floor(nextNormalized * physicalWidth)),
                    0LL,
                    static_cast<long long>(physicalWidth - 1));
                if (nextBucket != bucket) {
                    break;
                }
                if (points[bucketEnd].y < points[minimum].y) {
                    minimum = bucketEnd;
                }
                if (points[bucketEnd].y > points[maximum].y) {
                    maximum = bucketEnd;
                }
                ++bucketEnd;
            }

            appendBucket(
                selected, bucketStart, bucketEnd - 1, minimum, maximum);
            bucketStart = bucketEnd;
        }

        result.segments.reserve(result.segments.size() + selected.size());
        for (std::size_t index = 1; index < selected.size(); ++index) {
            appendSegment(
                result.segments,
                points[selected[index - 1]],
                points[selected[index]],
                xMinimum);
        }
        runStart = runEnd + 1;
    }
    return result;
}

} // namespace tnr::chart
