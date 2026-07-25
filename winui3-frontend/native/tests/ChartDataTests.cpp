#include "../src/chart/ChartData.h"

#include <cassert>
#include <cmath>
#include <limits>
#include <vector>

using tnr::chart::Point;
using tnr::chart::buildSegments;

int main() {
    {
        const std::vector<Point> points{{0, 1}, {1, 2}, {2, 3}};
        const auto result = buildSegments(points, 0, 2, 100);
        assert(!result.reduced);
        assert(result.segments.size() == 2);
    }
    {
        std::vector<Point> points;
        for (int index = 0; index < 1000; ++index) {
            points.push_back({static_cast<double>(index), index == 501 ? 5000.0 : 1.0});
        }
        const auto result = buildSegments(points, 0, 999, 100);
        assert(result.reduced);
        assert(result.segments.size() < 500);
        bool preservedSpike = false;
        for (const auto& segment : result.segments) {
            preservedSpike |= segment.y0 == 5000.0f || segment.y1 == 5000.0f;
        }
        assert(preservedSpike);
    }
    {
        const auto nan = std::numeric_limits<double>::quiet_NaN();
        const std::vector<Point> points{{0, 1}, {1, 2}, {2, nan}, {3, 3}, {4, 4}};
        const auto result = buildSegments(points, 0, 4, 100);
        assert(result.segments.size() == 2);
    }
    {
        const std::vector<Point> points{{0, 1}};
        assert(buildSegments(points, 0, 1, 100).segments.empty());
        assert(buildSegments(points, 1, 1, 100).segments.empty());
        assert(buildSegments(points, 0, 1, 0).segments.empty());
    }
}
