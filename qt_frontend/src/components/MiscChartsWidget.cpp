#include "MiscChartsWidget.h"
#include "ChartView.h"
#include "GraphTable.h"
#include "../SessionModel.h"

#include <QGridLayout>
#include <QColor>
#include <QTimer>
#include <QShowEvent>
#include <QStringList>
#include <algorithm>

namespace {
const QColor C_LAT("#F0A500"),   C_LONG("#5794F2");
const QColor C_FRONT("#73BF69"), C_REAR("#B877DB");
}

MiscChartsWidget::MiscChartsWidget(QWidget* parent)
    : QWidget(parent)
{
    outer_ = new QGridLayout(this);
    outer_->setContentsMargins(0, 0, 0, 0);
    outer_->setSpacing(ChartView::PanelGap);   // match the chart's inter-panel gap so
                                               // overlaid tables align with chart cells

    // Both sections are panels of one ChartView — a single QCustomPlot / GL context
    // / replot — each with its own in-plot title and colour key. Table-mode sections
    // render as GraphTables overlaid in the same grid cell (see rebuildLayout).
    chart_ = new ChartView;

    // ── G-FORCE (panel 0) ────────────────────────────────────────────────────
    xId_[GFORCE] = chart_->addAxis(
        { ChartView::Side::Bottom, 0.0, windowS_, QColor(), true, 'f', 1, true }, GFORCE);
    const int axG = chart_->addAxis(
        { ChartView::Side::Left, -6.0, 6.0, QColor(), true, 'f', 0 }, GFORCE);
    chart_->setAxisTimeTicker(xId_[GFORCE], "%m:%s");
    chart_->setAxisNumberSuffix(axG, 1.0, " G");
    chart_->setPanelTitle(GFORCE, "G-FORCE");
    chart_->setPanelLegendVisible(GFORCE, true);
    latId_  = chart_->addSeries({ "Lateral",      C_LAT,  2.0, xId_[GFORCE], axG, " G", 2 });
    longId_ = chart_->addSeries({ "Longitudinal", C_LONG, 2.0, xId_[GFORCE], axG, " G", 2 });

    // ── RIDE HEIGHT (panel 1) ────────────────────────────────────────────────
    chart_->addPanel();
    xId_[RIDEHEIGHT] = chart_->addAxis(
        { ChartView::Side::Bottom, 0.0, windowS_, QColor(), true, 'f', 1, true }, RIDEHEIGHT);
    const int axRH = chart_->addAxis(
        { ChartView::Side::Left, 0.0, 100.0, QColor(), true, 'f', 0 }, RIDEHEIGHT);
    chart_->setAxisTimeTicker(xId_[RIDEHEIGHT], "%m:%s");
    chart_->setAxisNumberSuffix(axRH, 1.0, " mm");
    chart_->setPanelTitle(RIDEHEIGHT, "RIDE HEIGHT");
    chart_->setPanelLegendVisible(RIDEHEIGHT, true);
    frontId_ = chart_->addSeries({ "Front", C_FRONT, 2.0, xId_[RIDEHEIGHT], axRH, " mm", 1 });
    rearId_  = chart_->addSeries({ "Rear",  C_REAR,  2.0, xId_[RIDEHEIGHT], axRH, " mm", 1 });

    chart_->setHoverReadout(true);
    rebuildLayout();

    // Zero-delay single-shot armed from requestRefresh(): coalesces to one rebuild
    // per event-loop pass (one per arriving packet, 20..60 Hz), no fixed rate cap.
    refreshTimer_ = new QTimer(this);
    refreshTimer_->setSingleShot(true);
    refreshTimer_->setInterval(0);
    connect(refreshTimer_, &QTimer::timeout, this, [this] {
        if (dirty_ && isVisible()) { dirty_ = false; refresh(); }
    });
}

void MiscChartsWidget::showEvent(QShowEvent* e) {
    QWidget::showEvent(e);
    requestRefresh();
}

void MiscChartsWidget::setModel(SessionModel* m) {
    model_ = m;
    if (!m) return;
    connect(m, &SessionModel::telemetryAppended, this, &MiscChartsWidget::requestRefresh);
    connect(m, &SessionModel::wasReset,          this, &MiscChartsWidget::requestRefresh);
    requestRefresh();
}

void MiscChartsWidget::setPlaybackMode(bool on) { playback_ = on; requestRefresh(); }
void MiscChartsWidget::setCurrentTime(float t)  { currentTime_ = t; requestRefresh(); }
void MiscChartsWidget::setWindowSeconds(float s) { windowS_ = s; prevEndTime_ = -9999.0f; requestRefresh(); }
void MiscChartsWidget::requestRefresh() { dirty_ = true; if (!refreshTimer_->isActive()) refreshTimer_->start(); }

void MiscChartsWidget::setSectionVisible(int section, bool on) {
    if (section < 0 || section >= SECTIONS) return;
    if (visible_[section] == on) return;
    visible_[section] = on;
    rebuildLayout();
}

void MiscChartsWidget::setSectionViewMode(int section, bool table) {
    if (section < 0 || section >= SECTIONS) return;
    if (tableMode_[section] == table) return;
    tableMode_[section] = table;
    rebuildLayout();
    requestRefresh();   // populate the freshly-shown table immediately
}

void MiscChartsWidget::ensureTable(int section) {
    if (table_[section]) return;
    QVector<GraphTable::Column> cols;
    switch (section) {
        case GFORCE:     cols = { { "Time", GraphTable::Time }, { "Lateral (G)", GraphTable::Fixed2 },
                                  { "Longitudinal (G)", GraphTable::Fixed2 } }; break;
        case RIDEHEIGHT: cols = { { "Time", GraphTable::Time }, { "Front (mm)", GraphTable::Fixed1 },
                                  { "Rear (mm)", GraphTable::Fixed1 } }; break;
    }
    table_[section] = new GraphTable(cols, this);
    table_[section]->setVisible(false);
}

void MiscChartsWidget::rebuildLayout() {
    if (!chart_ || !outer_) return;

    // G-force over ride-height, each a full-width row. Both chart- and table-mode
    // sections keep these positions; a table just replaces its chart in place.
    QVector<QVector<int>> rows;
    if (visible_[GFORCE])     rows.append(QVector<int>{ GFORCE });
    if (visible_[RIDEHEIGHT]) rows.append(QVector<int>{ RIDEHEIGHT });

    tnr::layoutSectionGrid(outer_, chart_, rows, SECTIONS, tableMode_, table_,
                           [this](int s) { ensureTable(s); });
}

float MiscChartsWidget::currentTime() const {
    if (playback_) return currentTime_;
    return model_ ? model_->data().latestTime : 0.0f;
}

void MiscChartsWidget::refresh() {
    if (!model_ || !chart_) return;

    const SessionData& d = model_->data();
    const float endTime = currentTime();

    // Seek (either direction) or reset: drop everything and re-scan from scratch.
    if (std::abs(endTime - prevEndTime_) > 1.0f || endTime < prevEndTime_) {
        chart_->clear(latId_);   chart_->clear(longId_);
        chart_->clear(frontId_); chart_->clear(rearId_);
        lastMotionT_   = -1.0f;
        lastMotionExT_ = -1.0f;
    }

    // G-force ← motionBuf.
    {
        const auto& buf = d.motionBuf;
        int start = 0;
        if (lastMotionT_ >= 0) {
            auto it = std::lower_bound(buf.begin(), buf.end(), lastMotionT_ + 0.0001f,
                [](const MotionSample& s, float t) { return s.t < t; });
            start = (int)std::distance(buf.begin(), it);
        }
        for (int i = start; i < buf.size(); ++i) {
            const auto& s = buf[i];
            if (s.t > endTime) break;
            chart_->appendPoint(latId_,  s.t, s.g_lat);
            chart_->appendPoint(longId_, s.t, s.g_long);
            lastMotionT_ = s.t;
        }
    }

    // Ride height ← motionExBuf.
    {
        const auto& buf = d.motionExBuf;
        int start = 0;
        if (lastMotionExT_ >= 0) {
            auto it = std::lower_bound(buf.begin(), buf.end(), lastMotionExT_ + 0.0001f,
                [](const MotionExSample& s, float t) { return s.t < t; });
            start = (int)std::distance(buf.begin(), it);
        }
        for (int i = start; i < buf.size(); ++i) {
            const auto& s = buf[i];
            if (s.t > endTime) break;
            chart_->appendPoint(frontId_, s.t, s.front_aero);
            chart_->appendPoint(rearId_,  s.t, s.rear_aero);
            lastMotionExT_ = s.t;
        }
    }

    const float startTime = qMax(0.0f, endTime - windowS_);
    chart_->trimBefore(latId_,   startTime);
    chart_->trimBefore(longId_,  startTime);
    chart_->trimBefore(frontId_, startTime);
    chart_->trimBefore(rearId_,  startTime);

    chart_->setXRange(xId_[GFORCE],     startTime, startTime + windowS_);
    chart_->setXRange(xId_[RIDEHEIGHT], startTime, startTime + windowS_);

    // Feed any table-mode sections from the same window (newest sample on top).
    if (tableMode_[GFORCE] && visible_[GFORCE] && table_[GFORCE]) {
        GraphTable* t = table_[GFORCE];
        t->beginRebuild();
        const auto& buf = d.motionBuf;
        for (int i = buf.size() - 1; i >= 0 && !t->full(); --i) {
            const auto& s = buf[i];
            if (s.t > endTime)   continue;
            if (s.t < startTime) break;
            t->addRow(s.t, s.g_lat, s.g_long);
        }
        t->endRebuild();
    }
    if (tableMode_[RIDEHEIGHT] && visible_[RIDEHEIGHT] && table_[RIDEHEIGHT]) {
        GraphTable* t = table_[RIDEHEIGHT];
        t->beginRebuild();
        const auto& buf = d.motionExBuf;
        for (int i = buf.size() - 1; i >= 0 && !t->full(); --i) {
            const auto& s = buf[i];
            if (s.t > endTime)   continue;
            if (s.t < startTime) break;
            t->addRow(s.t, s.front_aero, s.rear_aero);
        }
        t->endRebuild();
    }

    // ONE replot renders both panels — but skip it when the chart is hidden (every
    // section in table mode), so nothing renders off-screen. rebuildLayout() re-shows
    // it and requestRefresh()es on the way back to chart mode, so it repaints then.
    if (chart_->isVisible()) chart_->requestReplot();
    prevEndTime_ = endTime;
}
