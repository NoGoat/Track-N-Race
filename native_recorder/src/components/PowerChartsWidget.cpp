#include "PowerChartsWidget.h"
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
const QColor C_ICE("#5794F2"), C_MGUK("#FADE2A");
const QColor C_HARV_K("#37872D"), C_HARV_H("#C4162A");
const QColor C_FUEL("#F0A500");
}

PowerChartsWidget::PowerChartsWidget(QWidget* parent)
    : QWidget(parent)
{
    outer_ = new QGridLayout(this);
    outer_->setContentsMargins(0, 0, 0, 0);
    outer_->setSpacing(ChartView::PanelGap);   // match the chart's inter-panel gap so
                                               // overlaid tables align with chart cells

    // All four sections are panels of one ChartView — a single QCustomPlot / GL
    // context / replot. Each carries its own in-plot title (no colour key, matching
    // the old headers). All four read stsBuf. Table-mode sections render as
    // GraphTables overlaid in the same grid cell (see rebuildLayout).
    chart_ = new ChartView;

    auto timeAxis = [&](int sec) {
        xId_[sec] = chart_->addAxis(
            { ChartView::Side::Bottom, 0.0, windowS_, QColor(), true, 'f', 0, true }, sec);
        chart_->setAxisTimeTicker(xId_[sec], "%m:%s");
    };

    // ── POWER SPLIT (panel 0) ────────────────────────────────────────────────
    timeAxis(SPLIT);
    const int axSplit = chart_->addAxis({ ChartView::Side::Left, 0.0, 1000.0, QColor(), true, 'f', 0 }, SPLIT);
    chart_->setPanelTitle(SPLIT, "POWER SPLIT");
    chart_->setPanelLegendVisible(SPLIT, false);
    splitIceId_  = chart_->addSeries({ "ICE",   C_ICE,  1.5, xId_[SPLIT], axSplit, "", 2, false, true });
    splitMgukId_ = chart_->addSeries({ "MGU-K", C_MGUK, 1.5, xId_[SPLIT], axSplit, "", 2, false, true });

    // ── ERS HARVEST (panel 1) ────────────────────────────────────────────────
    chart_->addPanel();
    timeAxis(HARVEST);
    harvYId_ = chart_->addAxis({ ChartView::Side::Left, 0.0, 4000.0, QColor(), true, 'f', 0 }, HARVEST);
    chart_->setPanelTitle(HARVEST, "ERS HARVEST THIS LAP");
    chart_->setPanelLegendVisible(HARVEST, false);
    harvKId_ = chart_->addSeries({ "MGU-K Harvest", C_HARV_K, 1.5, xId_[HARVEST], harvYId_, "", 2, false, true });
    harvHId_ = chart_->addSeries({ "MGU-H Harvest", C_HARV_H, 1.5, xId_[HARVEST], harvYId_, "", 2, false, true });

    // ── ERS STORE (panel 2) ──────────────────────────────────────────────────
    chart_->addPanel();
    timeAxis(STORE);
    const int axStore = chart_->addAxis({ ChartView::Side::Left, 0.0, 100.0, QColor(), true, 'f', 0 }, STORE);
    chart_->setPanelTitle(STORE, "ERS STORE HISTORY");
    chart_->setPanelLegendVisible(STORE, false);
    storeId_ = chart_->addSeries({ "ERS Store", C_ICE, 1.5, xId_[STORE], axStore, "", 2, false, true });

    // ── FUEL (panel 3) ───────────────────────────────────────────────────────
    chart_->addPanel();
    timeAxis(FUEL);
    const int axFuel = chart_->addAxis({ ChartView::Side::Left, 0.0, 110.0, QColor(), true, 'f', 0 }, FUEL);
    chart_->setPanelTitle(FUEL, "FUEL HISTORY");
    chart_->setPanelLegendVisible(FUEL, false);
    fuelId_ = chart_->addSeries({ "Fuel", C_FUEL, 1.5, xId_[FUEL], axFuel, "", 2, false, true });

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

void PowerChartsWidget::showEvent(QShowEvent* e) {
    QWidget::showEvent(e);
    requestRefresh();
}

void PowerChartsWidget::setModel(SessionModel* m) {
    model_ = m;
    if (!m) return;
    connect(m, &SessionModel::telemetryAppended, this, &PowerChartsWidget::requestRefresh);
    connect(m, &SessionModel::wasReset,          this, &PowerChartsWidget::requestRefresh);
    requestRefresh();
}

void PowerChartsWidget::setPlaybackMode(bool on) { playback_ = on; requestRefresh(); }
void PowerChartsWidget::setCurrentTime(float t)  { currentTime_ = t; requestRefresh(); }
void PowerChartsWidget::setWindowSeconds(float s) { windowS_ = s; prevEndTime_ = -9999.0f; requestRefresh(); }
void PowerChartsWidget::requestRefresh() { dirty_ = true; if (!refreshTimer_->isActive()) refreshTimer_->start(); }

void PowerChartsWidget::applyHarvestScale(uint16_t format) {
    if (chart_ && harvYId_ >= 0)
        chart_->setXRange(harvYId_, 0.0, format >= 2026 ? 8000.0 : 4000.0);
}

void PowerChartsWidget::setSectionVisible(int section, bool on) {
    if (section < 0 || section >= SECTIONS) return;
    if (visible_[section] == on) return;
    visible_[section] = on;
    rebuildLayout();
}

void PowerChartsWidget::setSectionViewMode(int section, bool table) {
    if (section < 0 || section >= SECTIONS) return;
    if (tableMode_[section] == table) return;
    tableMode_[section] = table;
    rebuildLayout();
    requestRefresh();   // populate the freshly-shown table immediately
}

void PowerChartsWidget::ensureTable(int section) {
    if (table_[section]) return;
    QVector<GraphTable::Column> cols;
    switch (section) {
        case SPLIT:   cols = { { "Time", GraphTable::Time }, { "ICE (kW)", GraphTable::Fixed1 },
                               { "MGU-K (kW)", GraphTable::Fixed1 } }; break;
        case HARVEST: cols = { { "Time", GraphTable::Time }, { "MGU-K (kJ)", GraphTable::Fixed1 },
                               { "MGU-H (kJ)", GraphTable::Fixed1 } }; break;
        case STORE:   cols = { { "Time", GraphTable::Time }, { "ERS (%)", GraphTable::Fixed1 } }; break;
        case FUEL:    cols = { { "Time", GraphTable::Time }, { "Fuel (kg)", GraphTable::Fixed2 } }; break;
    }
    table_[section] = new GraphTable(cols, this);
    table_[section]->setVisible(false);
}

void PowerChartsWidget::rebuildLayout() {
    if (!chart_ || !outer_) return;
    // 2×2: split + harvest on top, store + fuel below; hidden panels drop out and
    // the survivors reflow (a lone section in a row spans the full width). Both
    // chart- and table-mode sections keep these positions; a table just replaces
    // its chart in place (see tnr::layoutSectionGrid).
    QVector<int> top, bottom;
    if (visible_[SPLIT])   top.append(SPLIT);
    if (visible_[HARVEST]) top.append(HARVEST);
    if (visible_[STORE])   bottom.append(STORE);
    if (visible_[FUEL])    bottom.append(FUEL);

    QVector<QVector<int>> rows;
    if (!top.isEmpty())    rows.append(top);
    if (!bottom.isEmpty()) rows.append(bottom);

    tnr::layoutSectionGrid(outer_, chart_, rows, SECTIONS, tableMode_, table_,
                           [this](int s) { ensureTable(s); });
}

float PowerChartsWidget::currentTime() const {
    if (playback_) return currentTime_;
    return model_ ? model_->data().latestTime : 0.0f;
}

void PowerChartsWidget::refresh() {
    if (!model_ || !chart_) return;

    const SessionData& d = model_->data();
    const float endTime = currentTime();
    const float left    = endTime - windowS_;

    if (std::abs(endTime - prevEndTime_) > 1.0f || endTime < prevEndTime_) {
        chart_->clear(splitIceId_); chart_->clear(splitMgukId_);
        chart_->clear(harvKId_);    chart_->clear(harvHId_);
        chart_->clear(storeId_);
        chart_->clear(fuelId_);
        lastAddedTime_ = left;
    }

    auto lb = [](const auto& v, float t) {
        return std::lower_bound(v.begin(), v.end(), t,
            [](const auto& s, float key) { return s.t < key; });
    };

    int startIdx = (int)std::distance(d.stsBuf.begin(), lb(d.stsBuf, lastAddedTime_ + 0.0001f));
    for (int i = startIdx; i < d.stsBuf.size(); ++i) {
        const auto& s = d.stsBuf[i];
        if (s.t > endTime) break;
        chart_->appendPoint(splitIceId_,  s.t, s.ice_kw);
        chart_->appendPoint(splitMgukId_, s.t, s.mguk_kw);
        chart_->appendPoint(harvKId_, s.t, s.mguk_harvest_j / 1000.0f);   // J → kJ
        chart_->appendPoint(harvHId_, s.t, s.mguh_harvest_j / 1000.0f);
        chart_->appendPoint(storeId_, s.t, s.ers);                        // ERS %
        chart_->appendPoint(fuelId_,  s.t, s.fuel_kg);
        lastAddedTime_ = s.t;
    }

    for (int id : { splitIceId_, splitMgukId_, harvKId_, harvHId_, storeId_, fuelId_ })
        chart_->trimBefore(id, left);

    const double lo = (double)std::max(0.0f, left);
    const double hi = (double)std::max(windowS_, endTime);
    const double hiClamped = hi > lo ? hi : lo + 1.0;
    for (int s = 0; s < SECTIONS; ++s)
        chart_->setXRange(xId_[s], lo, hiClamped);

    // Feed any table-mode sections from the same window (newest sample on top).
    if (tableMode_[SPLIT] || tableMode_[HARVEST] || tableMode_[STORE] || tableMode_[FUEL]) {
        GraphTable* tSplit   = (tableMode_[SPLIT]   && visible_[SPLIT])   ? table_[SPLIT]   : nullptr;
        GraphTable* tHarvest = (tableMode_[HARVEST] && visible_[HARVEST]) ? table_[HARVEST] : nullptr;
        GraphTable* tStore   = (tableMode_[STORE]   && visible_[STORE])   ? table_[STORE]   : nullptr;
        GraphTable* tFuel    = (tableMode_[FUEL]    && visible_[FUEL])    ? table_[FUEL]    : nullptr;
        for (GraphTable* t : { tSplit, tHarvest, tStore, tFuel }) if (t) t->beginRebuild();
        for (int i = d.stsBuf.size() - 1; i >= 0; --i) {
            const auto& s = d.stsBuf[i];
            if (s.t > endTime) continue;
            if (s.t < left)    break;
            if (tSplit && !tSplit->full())
                tSplit->addRow(s.t, s.ice_kw, s.mguk_kw);
            if (tHarvest && !tHarvest->full())
                tHarvest->addRow(s.t, s.mguk_harvest_j / 1000.0f, s.mguh_harvest_j / 1000.0f);
            if (tStore && !tStore->full())
                tStore->addRow(s.t, s.ers);
            if (tFuel && !tFuel->full())
                tFuel->addRow(s.t, s.fuel_kg);
            bool allFull = true;
            for (GraphTable* t : { tSplit, tHarvest, tStore, tFuel })
                if (t && !t->full()) { allFull = false; break; }
            if (allFull) break;
        }
        for (GraphTable* t : { tSplit, tHarvest, tStore, tFuel }) if (t) t->endRebuild();
    }

    // ONE replot renders all four panels — but skip it when the chart is hidden
    // (every section in table mode), so nothing renders off-screen. rebuildLayout()
    // re-shows and requestRefresh()es on the way back to chart mode, so it repaints then.
    if (chart_->isVisible()) chart_->requestReplot();
    prevEndTime_ = endTime;
}
