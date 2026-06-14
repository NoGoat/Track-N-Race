#pragma once

#include <QChartView>
#include <QList>
#include <QPointF>

class QChart;
class QLineSeries;
class QValueAxis;

// GPU-accelerated speed / RPM / ERS overlay built on Qt Charts. Replaces the
// hand-drawn QPainter chart; each series uses an OpenGL render path so paint
// cost no longer scales with the number of retained points.
class TelemetryChart : public QChartView {
    Q_OBJECT

public:
    explicit TelemetryChart(QWidget* parent = nullptr);

public slots:
    // Live append of a single sample (speed kph, rpm, ers %).
    void addPoint(float sessionTime, float speed, int rpm, float ers);

    // Bulk replace — used to refill history in one shot after a seek.
    void replaceAll(const QList<QPointF>& speed,
                    const QList<QPointF>& rpm,
                    const QList<QPointF>& ers,
                    float latestTime);

    void setWindowSeconds(float seconds);
    void reset();

protected:
    void changeEvent(QEvent* e) override;   // keep legend color in sync with the theme

private:
    static constexpr float MAX_SPEED = 380.0f;
    static constexpr float MAX_RPM   = 16000.0f;

    void rescaleX();
    void trim(QLineSeries* s, float cutoff);

    QChart*      chart_   = nullptr;
    QLineSeries* speedS_  = nullptr;
    QLineSeries* rpmS_    = nullptr;
    QLineSeries* ersS_    = nullptr;
    QValueAxis*  axX_     = nullptr;
    QValueAxis*  axRpm_   = nullptr;
    QValueAxis*  axSpeed_ = nullptr;
    QValueAxis*  axErs_   = nullptr;

    float windowS_ = 30.0f;
    float latestT_ = 0.0f;
};
