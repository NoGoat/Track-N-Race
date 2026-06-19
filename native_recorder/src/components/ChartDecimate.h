#pragma once

#include <QList>
#include <QPointF>

// Per-pixel min/max decimation, the replacement for QCustomPlot's
// setAdaptiveSampling(true). Qt Graphs has no built-in decimation, so the full
// sample set is kept in C++ and a reduced copy sized to the plot's pixel width is
// pushed to the QLineSeries on each replot.
//
// Points are assumed sorted by x (key). For each horizontal pixel bucket the
// min-y and max-y samples are emitted (in x order) so spikes/dips survive the
// reduction — a flat average would swallow them. The first and last points are
// always kept so the curve still spans the full range.
namespace ChartDecimate {

// targetBuckets is typically the plot width in pixels. When the input already
// fits (<= 2 points per bucket) it is returned unchanged.
QList<QPointF> minMaxByPixel(const QList<QPointF>& pts, int targetBuckets);

// Single-point-per-bucket variant for FILLED series. minMaxByPixel emits a
// vertical min/max pair per bucket, which for an area turns into self-overlapping
// triangles (visual corruption) and, on noisy data, a triangulation perf cliff.
// This keeps one sample per bucket — the one furthest from zero, so peaks/dips
// survive — yielding a clean, single-valued polygon.
QList<QPointF> peakByPixel(const QList<QPointF>& pts, int targetBuckets);

// Drop the interior points of flat runs (consecutive equal y), keeping each run's
// endpoints. Essential for filled series whose curve sits at exactly 0 for long
// stretches (throttle/brake): the area renderer starts a new subpath at every
// consecutive zero-pair, so an un-collapsed flat-zero run spawns hundreds of
// degenerate subpaths and tanks the fill's tessellation. Visually identical.
QList<QPointF> collapseFlats(const QList<QPointF>& pts);

// Enforce strictly-increasing x, keeping the latest y at each x. At session start
// the telemetry timestamps a burst of samples at t=0, producing hundreds of points
// stacked at the same x; that degenerate vertical fan breaks the area fill (wedge)
// and tanks its tessellation (lag) until the window scrolls past it. Assumes input
// sorted by x.
QList<QPointF> dropDuplicateX(const QList<QPointF>& pts);

}
