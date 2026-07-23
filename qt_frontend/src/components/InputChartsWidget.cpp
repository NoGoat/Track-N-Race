#include "InputChartsWidget.h"
#include "ChartView.h"
#include "GraphTable.h"
#include "../SessionModel.h"

#include <QGridLayout>
#include <QColor>
#include <QTimer>
#include <QShowEvent>
#include <QStringList>
#include <QLayoutItem>
#include <algorithm>

namespace {
const QColor C_GEAR("#5794F2");
const QColor C_THROTTLE("#2FC584"), C_BRAKE("#FF4040");
const QColor C_STEER("#BF5FFF");
}

InputChartsWidget::InputChartsWidget(QWidget* parent)
    : QWidget(parent)
{
    outer_ = new QGridLayout(this);
    outer_->setContentsMargins(0, 0, 0, 0);
    outer_->setSpacing(ChartView::PanelGap);   // match the chart's inter-panel gap so
                                               // overlaid tables align with chart cells

    // All three sections are panels of one ChartView — a single QCustomPlot / GL
    // context / replot. Each carries its own in-plot title (and colour key).
    // Table-mode sections render as GraphTables placed in the same grid geometry
    // as the charts (see rebuildLayout); chart_ is added to the grid there.
    chart_ = new ChartView;

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

void InputChartsWidget::setSectionViewMode(int section, bool table) {
    if (section < 0 || section >= SECTIONS) return;
    if (tableMode_[section] == table) return;
    tableMode_[section] = table;
    rebuildLayout();
    requestRefresh();   // populate the freshly-shown table immediately
}

void InputChartsWidget::ensureTable(int section) {
    if (table_[section]) return;
    QVector<GraphTable::Column> cols;
    switch (section) {
        case GEAR:     cols = { { "Time", GraphTable::Time }, { "Gear", GraphTable::Fixed0 } }; break;
        case INPUTS:   cols = { { "Time", GraphTable::Time }, { "Throttle", GraphTable::Fixed2 },
                                { "Brake", GraphTable::Fixed2 } }; break;
        case STEERING: cols = { { "Time", GraphTable::Time }, { "Steering", GraphTable::Fixed2 } }; break;
    }
    table_[section] = new GraphTable(cols, this);
    table_[section]->setVisible(false);
}

void InputChartsWidget::rebuildLayout() {
    if (!chart_ || !outer_) return;
    // Gear + throttle-brake share the top row; steering spans the full width below.
    // Both chart- and table-mode sections keep these natural positions — tables just
    // replace their chart in place (see tnr::layoutSectionGrid).
    QVector<QVector<int>> nat;   // natural grid rows of visible sections
    QVector<int> top;
    if (visible_[GEAR])   top.append(GEAR);
    if (visible_[INPUTS]) top.append(INPUTS);
    if (!top.isEmpty())     nat.append(top);
    if (visible_[STEERING]) nat.append(QVector<int>{ STEERING });

    tnr::layoutSectionGrid(outer_, chart_, nat, SECTIONS, tableMode_, table_,
                           [this](int s) { ensureTable(s); });
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

    // Feed any table-mode sections from the same window (newest sample on top).
    const bool anyTable = tableMode_[GEAR] || tableMode_[INPUTS] || tableMode_[STEERING];
    if (anyTable) {
        GraphTable* tg = (tableMode_[GEAR]     && visible_[GEAR])     ? table_[GEAR]     : nullptr;
        GraphTable* ti = (tableMode_[INPUTS]   && visible_[INPUTS])   ? table_[INPUTS]   : nullptr;
        GraphTable* ts = (tableMode_[STEERING] && visible_[STEERING]) ? table_[STEERING] : nullptr;
        if (tg) tg->beginRebuild();
        if (ti) ti->beginRebuild();
        if (ts) ts->beginRebuild();
        for (int i = d.telBuf.size() - 1; i >= 0; --i) {
            const auto& s = d.telBuf[i];
            if (s.t > endTime) continue;
            if (s.t < left)    break;
            if (tg && !tg->full())
                tg->addRow(s.t, s.gear);
            if (ti && !ti->full())
                ti->addRow(s.t, s.throttle, s.brake);
            if (ts && !ts->full())
                ts->addRow(s.t, s.steering);
            if ((!tg || tg->full()) && (!ti || ti->full()) && (!ts || ts->full())) break;
        }
        if (tg) tg->endRebuild();
        if (ti) ti->endRebuild();
        if (ts) ts->endRebuild();
    }

    // ONE replot renders all three panels — but skip it when the chart is hidden
    // (every section in table mode), so nothing renders off-screen. rebuildLayout()
    // re-shows and requestRefresh()es on the way back to chart mode, so it repaints then.
    if (chart_->isVisible()) chart_->requestReplot();
    prevEndTime_ = endTime;
}
