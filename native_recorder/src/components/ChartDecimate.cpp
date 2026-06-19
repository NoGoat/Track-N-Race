#include "ChartDecimate.h"

#include <cmath>

namespace ChartDecimate {

QList<QPointF> minMaxByPixel(const QList<QPointF>& pts, int targetBuckets)
{
    const int n = pts.size();
    if (targetBuckets <= 0 || n <= targetBuckets * 2)
        return pts;

    QList<QPointF> out;
    out.reserve(targetBuckets * 2 + 2);
    out.append(pts.first());

    // Walk fixed index ranges so the cost is O(n) regardless of x distribution.
    // Each bucket contributes its lowest and highest sample, ordered by x so the
    // emitted polyline keeps moving left-to-right.
    for (int b = 0; b < targetBuckets; ++b) {
        const int lo = int(qint64(b)     * n / targetBuckets);
        const int hi = int(qint64(b + 1) * n / targetBuckets);
        if (lo >= hi) continue;

        int minI = lo, maxI = lo;
        for (int i = lo + 1; i < hi; ++i) {
            if (pts[i].y() < pts[minI].y()) minI = i;
            if (pts[i].y() > pts[maxI].y()) maxI = i;
        }
        if (minI <= maxI) { out.append(pts[minI]); if (maxI != minI) out.append(pts[maxI]); }
        else              { out.append(pts[maxI]); out.append(pts[minI]); }
    }

    if (pts.last() != out.last())
        out.append(pts.last());
    return out;
}

QList<QPointF> peakByPixel(const QList<QPointF>& pts, int targetBuckets)
{
    const int n = pts.size();
    if (targetBuckets <= 0 || n <= targetBuckets)
        return pts;

    QList<QPointF> out;
    out.reserve(targetBuckets + 2);
    out.append(pts.first());

    for (int b = 0; b < targetBuckets; ++b) {
        const int lo = int(qint64(b)     * n / targetBuckets);
        const int hi = int(qint64(b + 1) * n / targetBuckets);
        if (lo >= hi) continue;

        int pick = lo;
        for (int i = lo + 1; i < hi; ++i)
            if (std::abs(pts[i].y()) > std::abs(pts[pick].y())) pick = i;
        out.append(pts[pick]);
    }

    if (pts.last() != out.last())
        out.append(pts.last());
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
