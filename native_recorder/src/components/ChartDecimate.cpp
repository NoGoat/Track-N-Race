#include "ChartDecimate.h"

#include <cmath>

namespace ChartDecimate {

namespace {
// Absolute-grid bucket id for a sample's x. Fixed for a given x regardless of how
// the window pans, so historical buckets never reshuffle (no shimmer).
inline long long bucketOf(double x, double bucketWidth)
{
    return static_cast<long long>(std::floor(x / bucketWidth));
}
}

QList<QPointF> minMaxByPixel(const QList<QPointF>& pts, double bucketWidth)
{
    const int n = pts.size();
    if (bucketWidth <= 0.0 || n <= 2)
        return pts;

    QList<QPointF> out;
    out.reserve(n);
    out.append(pts.first());

    // pts are sorted by x, so walk once and group consecutive samples sharing a
    // bucket. Each bucket contributes its lowest and highest sample, ordered by x.
    int i = 0;
    while (i < n) {
        const long long b = bucketOf(pts[i].x(), bucketWidth);
        int minI = i, maxI = i, j = i + 1;
        for (; j < n && bucketOf(pts[j].x(), bucketWidth) == b; ++j) {
            if (pts[j].y() < pts[minI].y()) minI = j;
            if (pts[j].y() > pts[maxI].y()) maxI = j;
        }
        if (minI <= maxI) { out.append(pts[minI]); if (maxI != minI) out.append(pts[maxI]); }
        else              { out.append(pts[maxI]); out.append(pts[minI]); }
        i = j;
    }

    if (pts.last() != out.last())
        out.append(pts.last());
    return out;
}

QList<QPointF> peakByPixel(const QList<QPointF>& pts, double bucketWidth)
{
    const int n = pts.size();
    if (bucketWidth <= 0.0 || n <= 2)
        return pts;

    QList<QPointF> out;
    out.reserve(n);
    out.append(pts.first());

    int i = 0;
    while (i < n) {
        const long long b = bucketOf(pts[i].x(), bucketWidth);
        int pick = i, j = i + 1;
        for (; j < n && bucketOf(pts[j].x(), bucketWidth) == b; ++j)
            if (std::abs(pts[j].y()) > std::abs(pts[pick].y())) pick = j;
        out.append(pts[pick]);
        i = j;
    }

    if (pts.last() != out.last())
        out.append(pts.last());
    return out;
}

QList<QPointF> dropDuplicateX(const QList<QPointF>& pts)
{
    if (pts.size() < 2) return pts;
    QList<QPointF> out;
    out.reserve(pts.size());
    out.append(pts.first());
    for (int i = 1; i < pts.size(); ++i) {
        if (pts[i].x() > out.last().x()) out.append(pts[i]);
        else                             out.last() = pts[i];   // same x → keep latest y
    }
    return out;
}

QList<QPointF> collapseFlats(const QList<QPointF>& pts)
{
    const int n = pts.size();
    if (n <= 2) return pts;

    QList<QPointF> out;
    out.reserve(n);
    out.append(pts.first());
    for (int i = 1; i < n - 1; ++i) {
        // Keep only the endpoints of a flat run — drop a point whose neighbours
        // both share its y. Run boundaries (where y changes) are always kept.
        if (pts[i].y() == pts[i - 1].y() && pts[i].y() == pts[i + 1].y())
            continue;
        out.append(pts[i]);
    }
    out.append(pts.last());
    return out;
}

}
