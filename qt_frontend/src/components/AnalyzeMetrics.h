#pragma once

#include <QColor>
#include <QString>
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

struct AnalyzeSeriesSetting {
    QString metricId;
    QColor color;
    bool visible = true;
};
