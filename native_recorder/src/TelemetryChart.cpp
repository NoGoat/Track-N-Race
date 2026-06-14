#include "TelemetryChart.h"

#include <QChart>
#include <QLineSeries>
#include <QValueAxis>
#include <QPen>
#include <QColor>
#include <QPainter>

TelemetryChart::TelemetryChart(QWidget* parent)
    : QChartView(parent)
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setMinimumHeight(120);
    setRenderHint(QPainter::Antialiasing);

    chart_ = new QChart;
    chart_->setAnimationOptions(QChart::NoAnimation);   // realtime: never animate
    chart_->setBackgroundVisible(false);
    chart_->setMargins(QMargins(0, 0, 0, 0));
    chart_->legend()->setVisible(true);
    chart_->legend()->setAlignment(Qt::AlignTop);

    speedS_ = new QLineSeries(chart_);  speedS_->setName("Speed");
    rpmS_   = new QLineSeries(chart_);  rpmS_->setName("RPM");
    ersS_   = new QLineSeries(chart_);  ersS_->setName("ERS");

    speedS_->setColor(QColor("#37872D"));
    rpmS_->setColor(QColor("#C4162A"));
    ersS_->setColor(QColor("#FADE2A"));

    // OpenGL render path — paint cost is independent of point count.
    speedS_->setUseOpenGL(true);
    rpmS_->setUseOpenGL(true);
    ersS_->setUseOpenGL(true);

    // Add ERS first so it sits behind RPM/Speed (later series draw on top).
    chart_->addSeries(ersS_);
    chart_->addSeries(rpmS_);
    chart_->addSeries(speedS_);

    axX_     = new QValueAxis(chart_);
    axRpm_   = new QValueAxis(chart_);
    axSpeed_ = new QValueAxis(chart_);
    axErs_   = new QValueAxis(chart_);

    axX_->setLabelFormat("%.0f");
    axX_->setTickCount(7);
    axX_->setTitleVisible(false);

    axRpm_->setRange(0, MAX_RPM);
    axRpm_->setLabelFormat("%.0f");
    axRpm_->setLabelsColor(QColor("#C4162A"));
    axRpm_->setTickCount(5);

    axSpeed_->setRange(0, MAX_SPEED);
    axSpeed_->setLabelFormat("%.0f");
    axSpeed_->setLabelsColor(QColor("#37872D"));
    axSpeed_->setTickCount(5);

    axErs_->setRange(0, 100);
    axErs_->setVisible(false);   // ERS shares the plot area but needs no labels

    chart_->addAxis(axX_,     Qt::AlignBottom);
    chart_->addAxis(axRpm_,   Qt::AlignLeft);
    chart_->addAxis(axSpeed_, Qt::AlignRight);
    chart_->addAxis(axErs_,   Qt::AlignRight);

    ersS_->attachAxis(axX_);   ersS_->attachAxis(axErs_);
    rpmS_->attachAxis(axX_);   rpmS_->attachAxis(axRpm_);
    speedS_->attachAxis(axX_); speedS_->attachAxis(axSpeed_);

    rescaleX();
    setChart(chart_);
}

void TelemetryChart::trim(QLineSeries* s, float cutoff) {
    int drop = 0;
    while (drop < s->count() && s->at(drop).x() < cutoff) ++drop;
    if (drop > 0) s->removePoints(0, drop);
}

void TelemetryChart::addPoint(float sessionTime, float speed, int rpm, float ers) {
    speedS_->append(sessionTime, speed);
    rpmS_->append(sessionTime, rpm);
    ersS_->append(sessionTime, ers);

    const float cutoff = sessionTime - MAX_WINDOW_S;
    trim(speedS_, cutoff);
    trim(rpmS_,   cutoff);
    trim(ersS_,   cutoff);

    latestT_ = sessionTime;
    rescaleX();
}

void TelemetryChart::replaceAll(const QList<QPointF>& speed,
                                const QList<QPointF>& rpm,
                                const QList<QPointF>& ers,
                                float latestTime) {
    speedS_->replace(speed);
    rpmS_->replace(rpm);
    ersS_->replace(ers);
    latestT_ = latestTime;
    rescaleX();
}

void TelemetryChart::setWindowSeconds(float seconds) {
    windowS_ = seconds;
    rescaleX();
}

void TelemetryChart::reset() {
    speedS_->clear();
    rpmS_->clear();
    ersS_->clear();
    latestT_ = 0.0f;
    rescaleX();
}

void TelemetryChart::rescaleX() {
    axX_->setRange(latestT_ - windowS_, latestT_);
}
