#include "InputChartsWidget.h"
#include "ChartView.h"
#include "../SessionModel.h"

#include <QVBoxLayout>
#include <QColor>
#include <QTimer>
#include <QShowEvent>
#include <algorithm>

namespace {
const QColor C_GEAR("#5794F2");
const QColor C_THROTTLE("#2FC584"), C_BRAKE("#FF4040");
const QColor C_STEER("#BF5FFF");
}

InputChartsWidget::InputChartsWidget(QWidget* parent)
    : QWidget(parent)
{
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    // All three sections are panels of one ChartView — a single QCustomPlot / GL
    // context / replot. Each carries its own in-plot title (and colour key).
    chart_ = new ChartView;
    outer->addWidget(chart_, 1);

    // ── GEAR (panel 0, the ChartView's default panel) ────────────────────────
    xId_[GEAR] = chart_->addAxis(
        { ChartView::Side::Bottom, 0.0, windowS_, QColor(), true, 'f', 0, true }, GEAR);
    const int axGear = chart_->addAxis(
        { ChartView::Side::Left, 0.0, 9.0, QColor(), true, 'f', 0 }, GEAR);
    chart_->setAxisTimeTicker(xId_[GEAR], "%m:%s");
    // Gear-range bands (bands target panel 0's shared key axis — gear is panel 0).
    chart_->addBand({ axGear, 0.0, 2.5, QColor(196, 22, 42, 51) });
    chart_->addBand({ axGear, 2.5, 4.5, QColor(212, 173, 4, 51) });
    chart_->addBand({ axGear, 4.5, 6.5, QColor(0, 200, 83, 51) });
    chart_->addBand({ axGear, 6.5, 9.0, QColor(31, 96, 196, 51) });
    chart_->setPanelTitle(GEAR, "GEAR INDICATOR");
    chart_->setPanelLegendVisible(GEAR, false);   // no colour key, like the old header
    gearId_ = chart_->addSeries(
        { "Gear", C_GEAR, 2.0, xId_[GEAR], axGear, "", 0, false, false, QColor(), true });

    // ── THROTTLE / BRAKE (panel 1) ───────────────────────────────────────────
    chart_->addPanel();
    xId_[INPUTS] = chart_->addAxis(
        { ChartView::Side::Bottom, 0.0, windowS_, QColor(), true, 'f', 0, true }, INPUTS);
    const int axPctIn = chart_->addAxis(
        { ChartView::Side::Left, -1.0, 1.0, QColor(), true, 'f', 2 }, INPUTS);
    chart_->setAxisTimeTicker(xId_[INPUTS], "%m:%s");
    chart_->setPanelTitle(INPUTS, "THROTTLE / BRAKE CHART");
    chart_->setPanelLegendVisible(INPUTS, true);
    thId_ = chart_->addSeries({ "Throttle", C_THROTTLE, 2.0, xId_[INPUTS], axPctIn, "", 2, false, true });
    brId_ = chart_->addSeries({ "Brake",    C_BRAKE,    2.0, xId_[INPUTS], axPctIn, "", 2, false, true });

    // ── STEERING (panel 2) ───────────────────────────────────────────────────
    chart_->addPanel();
    xId_[STEERING] = chart_->addAxis(
        { ChartView::Side::Bottom, 0.0, windowS_, QColor(), true, 'f', 0, true }, STEERING);
    const int axPctSt = chart_->addAxis(
        { ChartView::Side::Left, -1.0, 1.0, QColor(), true, 'f', 2 }, STEERING);
    chart_->setAxisTimeTicker(xId_[STEERING], "%m:%s");
    // The "- Left / + Right" hint (a colourless header note in the old layout) is
    // folded into the title, since the in-plot legend only carries series entries.
    chart_->setPanelTitle(STEERING, "STEERING TELEMETRY   ( - Left / + Right )");
    chart_->setPanelLegendVisible(STEERING, true);
    stId_ = chart_->addSeries({ "Steering", C_STEER, 2.0, xId_[STEERING], axPctSt, "", 2, false });

    chart_->setHoverReadout(true);   // once all panels/axes exist (per-panel crosshairs)
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

void InputChartsWidget::showEvent(QShowEvent* e) {
    QWidget::showEvent(e);
    requestRefresh();
}

void InputChartsWidget::setModel(SessionModel* m) {
    model_ = m;
    if (!m) return;
    connect(m, &SessionModel::telemetryAppended, this, &InputChartsWidget::requestRefresh);
    connect(m, &SessionModel::wasReset,          this, &InputChartsWidget::requestRefresh);
    requestRefresh();
}

void InputChartsWidget::setPlaybackMode(bool on) { playback_ = on; requestRefresh(); }
void InputChartsWidget::setCurrentTime(float t)  { currentTime_ = t; requestRefresh(); }
void InputChartsWidget::setWindowSeconds(float s) { windowS_ = s; prevEndTime_ = -9999.0f; requestRefresh(); }
void InputChartsWidget::requestRefresh() { dirty_ = true; if (!refreshTimer_->isActive()) refreshTimer_->start(); }

void InputChartsWidget::setSectionVisible(int section, bool on) {
    if (section < 0 || section >= SECTIONS) return;
    if (visible_[section] == on) return;
    visible_[section] = on;
    rebuildLayout();
}

void InputChartsWidget::rebuildLayout() {
    if (!chart_) return;
    // Gear + throttle-brake share the top row; steering spans the full width below.
    QVector<int> top;
    if (visible_[GEAR])   top.append(GEAR);
    if (visible_[INPUTS]) top.append(INPUTS);

    QVector<QVector<int>> rows;
    if (!top.isEmpty())       rows.append(top);
    if (visible_[STEERING])   rows.append(QVector<int>{ STEERING });
    chart_->layoutPanelsRows(rows);
}

float InputChartsWidget::currentTime() const {
    if (playback_) return currentTime_;
    return model_ ? model_->data().latestTime : 0.0f;
}

void InputChartsWidget::refresh() {
    if (!model_ || !chart_) return;

    const SessionData& d = model_->data();
    const float endTime = currentTime();
    const float left    = endTime - windowS_;

    if (std::abs(endTime - prevEndTime_) > 1.0f || endTime < prevEndTime_) {
        chart_->clear(gearId_);
        chart_->clear(thId_); chart_->clear(brId_);
        chart_->clear(stId_);
        lastAddedTime_ = left;
    }

    auto lb = [](const auto& v, float t) {
        return std::lower_bound(v.begin(), v.end(), t,
            [](const auto& s, float key) { return s.t < key; });
    };

    int startIdx = (int)std::distance(d.telBuf.begin(), lb(d.telBuf, lastAddedTime_ + 0.0001f));
    for (int i = startIdx; i < d.telBuf.size(); ++i) {
        const auto& s = d.telBuf[i];
        if (s.t > endTime) break;
        chart_->appendPoint(gearId_, s.t, s.gear);
        chart_->appendPoint(thId_,   s.t, s.throttle);
        chart_->appendPoint(brId_,   s.t, -s.brake);      // brake drawn below zero
        chart_->appendPoint(stId_,   s.t, s.steering);
        lastAddedTime_ = s.t;
    }

    chart_->trimBefore(gearId_, left);
    chart_->trimBefore(thId_,   left);
    chart_->trimBefore(brId_,   left);
    chart_->trimBefore(stId_,   left);

    const double lo = (double)std::max(0.0f, left);
    const double hi = (double)std::max(windowS_, endTime);
    const double hiClamped = hi > lo ? hi : lo + 1.0;
    for (int s = 0; s < SECTIONS; ++s)
        chart_->setXRange(xId_[s], lo, hiClamped);

    chart_->requestReplot();   // ONE replot renders all three panels
    prevEndTime_ = endTime;
}
