#include "TelemetryChart.h"

#include <QChart>
#include <QLineSeries>
#include <QValueAxis>
#include <QPen>
#include <QColor>
#include <QPainter>
#include <QEvent>

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
    chart_->legend()->setLabelColor(palette().color(QPalette::Text));

    speedS_ = new QLineSeries(chart_);  speedS_->setName("Speed");
    rpmS_   = new QLineSeries(chart_);  rpmS_->setName("RPM");
    ersS_   = new QLineSeries(chart_);  ersS_->setName("ERS");

    speedS_->setPen(QPen(QColor("#37872D"), 2.5));
    rpmS_->setPen(QPen(QColor("#C4162A"),   2.5));
    ersS_->setPen(QPen(QColor("#FADE2A"),   2.5));

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
    axX_->setGridLineVisible(false);

    axRpm_->setRange(0, MAX_RPM);
    axRpm_->setLabelFormat("%.0f");
    axRpm_->setLabelsColor(QColor("#C4162A"));
    axRpm_->setTickCount(5);
    axRpm_->setGridLineVisible(false);

    axSpeed_->setRange(0, MAX_SPEED);
    axSpeed_->setLabelFormat("%.0f");
    axSpeed_->setLabelsColor(QColor("#37872D"));
    axSpeed_->setTickCount(5);
    axSpeed_->setGridLineVisible(false);

    axErs_->setRange(0, 100);
    axErs_->setVisible(false);   // ERS shares the plot area but needs no labels
    axErs_->setGridLineVisible(false);

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

    // Retain only the visible window (plus a small margin so the line reaches the
    // left edge). On the CPU render path, paint cost scales with retained points,
    // so keeping more than is shown would re-introduce the old lag.
    const float cutoff = sessionTime - windowS_ - 2.0f;
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

    const float cutoff = latestTime - windowS_ - 2.0f;
    trim(speedS_, cutoff);
    trim(rpmS_,   cutoff);
    trim(ersS_,   cutoff);

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
    if (speedS_->count() == 0) {
        axX_->setRange(0, windowS_);
        return;
    }
    // Show at most windowS_ of data, but never pad past the earliest sample we
    // hold — so a 10m window over 6m of data shows 6m, not 4m of blank.
    const float earliest = speedS_->at(0).x();
    float left = latestT_ - windowS_;
    if (earliest > left) left = earliest;
    if (left >= latestT_) left = latestT_ - 1.0f;   // keep a positive span
    axX_->setRange(left, latestT_);
}

void TelemetryChart::changeEvent(QEvent* e) {
    QChartView::changeEvent(e);
    if (chart_ && (e->type() == QEvent::PaletteChange ||
                   e->type() == QEvent::ApplicationPaletteChange)) {
        chart_->legend()->setLabelColor(palette().color(QPalette::Text));
    }
}
