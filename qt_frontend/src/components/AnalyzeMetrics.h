#pragma once

#include <QColor>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

enum class AnalyzeSource { Telemetry, Motion, MotionEx, Status, Tyre, Damage };

struct AnalyzeMetric {
    QString id;
    QString group;
    QString label;
    AnalyzeSource source;
    QString field;
    QColor defaultColor;
    QString scaleKey;
    double min = 0;
    double max = 1;
    QString unit;
    int precision = 0;
    bool step = false;
};

const QVector<AnalyzeMetric>& analyzeMetrics();
const AnalyzeMetric* analyzeMetric(const QString& id);
// Picker order of the metric groups.
const QStringList& analyzeMetricGroups();

// ── Per-corner tyre metrics ─────────────────────────────────────────────────
// Corner metric ids are "<idPrefix>-<corner key>" (e.g. "surface-fl"). A tyre
// row can also be charted *combined*: one series "<idPrefix>-all" that draws
// its picked corners on one shared scale, in the corner colours. A combined
// series and its own corners are mutually exclusive in a configuration.

struct AnalyzeTyreCorner {
    QString key;     // fl, fr, rl, rr
    QString label;   // FL, FR, RL, RR
};

struct AnalyzeTyreRow {
    QString idPrefix;
    QString label;        // "Surface Temp"
    QString shortLabel;   // "Surface" — names the combined card
    QColor combinedColor; // the combined card's own colour: its title and axis
    QString combinedId() const { return idPrefix + QStringLiteral("-all"); }
};

const QVector<AnalyzeTyreCorner>& analyzeTyreCorners();
const QVector<AnalyzeTyreRow>& analyzeTyreRows();
// The row a combined id ("surface-all") stands for, or null.
const AnalyzeTyreRow* analyzeCombinedRow(const QString& seriesId);
// The row owning a corner metric ("surface-fl"), with its corner key, or null.
const AnalyzeTyreRow* analyzeCornerRow(const QString& metricId, QString* cornerKey = nullptr);
// The metric whose scale, unit and source a series uses: the metric itself,
// or the first corner of a combined series.
const AnalyzeMetric* analyzeScaleMetric(const QString& seriesId);
// Series that cannot be charted together with `seriesId`.
QStringList analyzeSeriesConflicts(const QString& seriesId);
// Valid corner keys in FL, FR, RL, RR order.
QStringList analyzeSanitizeCorners(const QStringList& corners);

struct AnalyzeSeriesSetting {
    QString metricId;
    QColor color;
    QColor negativeColor;
    bool visible = true;
    bool showYAxis = true;
    // Combined tyre series only: the corners drawn, and custom corner colours.
    QStringList corners;
    QHash<QString, QColor> cornerColors;
};

// Whether a series has anything to draw (a combined series may have no corners).
bool analyzeSeriesHasLines(const AnalyzeSeriesSetting& setting);
// The colour a series draws one of its member metrics in.
QColor analyzeSeriesLineColor(const AnalyzeSeriesSetting& setting, const QString& memberId);
