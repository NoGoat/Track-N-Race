#include "PowerChartsWidget.h"
#include "../PresentationScheduler.h"
#include "ChartView.h"
#include "GraphTable.h"
#include "../SessionModel.h"
#include "../ChartCoordinates.h"

#include <QGridLayout>
#include <QColor>
#include <QShowEvent>
#include <QStringList>
#include <QtMath>
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

    // All four sections are panels of one ChartView — a single QRhi render target /
    // context / replot. Each carries its own in-plot title (no colour key, matching
    // the old headers). All four read stsBuf. Table-mode sections render as
    // GraphTables overlaid in the same grid cell (see rebuildLayout).
    chart_ = new ChartView;

    auto timeAxis = [&](int sec) {
        xId_[sec] = chart_->addAxis(
            { ChartView::Side::Bottom, 0.0, windowS_, QColor(), true, 'f', 0, true }, sec);
        chart_->setAxisTimeTicker(xId_[sec], "%m:%s");
    };

    // ── POWER (panel 0) ──────────────────────────────────────────────────────
    timeAxis(SPLIT);
    const int axSplit = chart_->addAxis({ ChartView::Side::Left, 0.0, 500.0, QColor(), true, 'f', 0 }, SPLIT);
    chart_->setPanelTitle(SPLIT, "POWER");
    chart_->setPanelLegendVisible(SPLIT, true);
    splitIceId_  = chart_->addSeries({ "ICE",   C_ICE,  1.5, xId_[SPLIT], axSplit, "", 2 });
    splitMgukId_ = chart_->addSeries({ "MGU-K", C_MGUK, 1.5, xId_[SPLIT], axSplit, "", 2 });
    QColor iceRef = C_ICE; iceRef.setAlpha(105); QColor mgukRef = C_MGUK; mgukRef.setAlpha(105);
    splitIceRefId_ = chart_->addSeries({ "", iceRef, 1.1, xId_[SPLIT], axSplit, "", 2 });
    splitMgukRefId_ = chart_->addSeries({ "", mgukRef, 1.1, xId_[SPLIT], axSplit, "", 2 });

    // ── ERS HARVEST (panel 1) ────────────────────────────────────────────────
    chart_->addPanel();
    timeAxis(HARVEST);
    harvYId_ = chart_->addAxis({ ChartView::Side::Left, 0.0, 4000.0, QColor(), true, 'f', 0 }, HARVEST);
    chart_->setPanelTitle(HARVEST, "ERS HARVEST THIS LAP");
    chart_->setPanelLegendVisible(HARVEST, true);
    harvKId_ = chart_->addSeries({ "MGU-K Harvest", C_HARV_K, 1.5, xId_[HARVEST], harvYId_, "", 2 });
    harvHId_ = chart_->addSeries({ "MGU-H Harvest", C_HARV_H, 1.5, xId_[HARVEST], harvYId_, "", 2 });
    QColor hkRef = C_HARV_K; hkRef.setAlpha(105); QColor hhRef = C_HARV_H; hhRef.setAlpha(105);
    harvKRefId_ = chart_->addSeries({ "", hkRef, 1.1, xId_[HARVEST], harvYId_, "", 2 });
    harvHRefId_ = chart_->addSeries({ "", hhRef, 1.1, xId_[HARVEST], harvYId_, "", 2 });

    // ── ERS STORE (panel 2) ──────────────────────────────────────────────────
    chart_->addPanel();
    timeAxis(STORE);
    const int axStore = chart_->addAxis({ ChartView::Side::Left, 0.0, 100.0, QColor(), true, 'f', 0 }, STORE);
    chart_->setPanelTitle(STORE, "ERS STORE HISTORY");
    chart_->setPanelLegendVisible(STORE, true);
    storeId_ = chart_->addSeries({ "ERS Store", C_ICE, 1.5, xId_[STORE], axStore, "", 2 });
    storeRefId_ = chart_->addSeries({ "", iceRef, 1.1, xId_[STORE], axStore, "", 2 });

    // ── FUEL (panel 3) ───────────────────────────────────────────────────────
    chart_->addPanel();
    timeAxis(FUEL);
    const int axFuel = chart_->addAxis({ ChartView::Side::Left, 0.0, 110.0, QColor(), true, 'f', 0 }, FUEL);
    chart_->setPanelTitle(FUEL, "FUEL HISTORY");
    chart_->setPanelLegendVisible(FUEL, true);
    fuelId_ = chart_->addSeries({ "Fuel", C_FUEL, 1.5, xId_[FUEL], axFuel, "", 2 });
    QColor fuelRef = C_FUEL; fuelRef.setAlpha(105);
    fuelRefId_ = chart_->addSeries({ "", fuelRef, 1.1, xId_[FUEL], axFuel, "", 2 });
    const int primary[] = { splitIceId_, splitMgukId_, harvKId_, harvHId_, storeId_, fuelId_ };
    const int refs[] = { splitIceRefId_, splitMgukRefId_, harvKRefId_, harvHRefId_, storeRefId_, fuelRefId_ };
    for (int i = 0; i < 6; ++i) { chart_->setSeriesVisible(refs[i], false); chart_->linkSeriesVisibility(primary[i], refs[i]); }

    chart_->setHoverReadout(true);
    rebuildLayout();

    // Zero-delay single-shot armed from requestRefresh(): coalesces to one rebuild
    // per event-loop pass (one per arriving packet, 20..60 Hz), no fixed rate cap.
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
    connect(m, &SessionModel::chartConfigurationChanged, this, &PowerChartsWidget::requestRefresh);
    const tnr::GraphSection sections[SECTIONS] = { tnr::GraphSection::PowerSplit,
        tnr::GraphSection::PowerHarvest, tnr::GraphSection::PowerStore, tnr::GraphSection::PowerFuel };
    for (int i = 0; i < SECTIONS; ++i) chart_->bindPanelChartSettings(i, m, sections[i]);
    requestRefresh();
}

void PowerChartsWidget::setPlaybackMode(bool on) { playback_ = on; requestRefresh(); }
void PowerChartsWidget::setCurrentTime(float t)  { currentTime_ = t; requestRefresh(); }
void PowerChartsWidget::setWindowSeconds(float s) { windowS_ = s; prevEndTime_ = -9999.0f; requestRefresh(); }
void PowerChartsWidget::requestRefresh() {
    dirty_ = true;
    if (!isVisible()) return;
    PresentationScheduler::instance().request(this, [this] {
        if (dirty_ && isVisible()) { dirty_ = false; refresh(); }
    }, PresentationScheduler::Policy::Chart);
}

void PowerChartsWidget::applyHarvestScale(uint16_t format) {
    harvestFixedMax_ = format >= 2026 ? 8000.0 : 4000.0;
    requestRefresh();
}

void PowerChartsWidget::setMguhVisible(bool visible) {
    if (mguhVisible_ == visible) return;
    mguhVisible_ = visible;
    if (chart_ && harvHId_ >= 0) {
        chart_->setSeriesVisible(harvHId_, visible);
        chart_->setSeriesVisible(harvHRefId_, visible);
        chart_->clear(harvHId_);
    }
    if (table_[HARVEST]) {
        QVector<GraphTable::Column> cols = {
            { "Time", GraphTable::Time }, { "MGU-K (kJ)", GraphTable::Fixed1 }
        };
        if (visible) cols.append(GraphTable::Column{ "MGU-H (kJ)", GraphTable::Fixed1 });
        table_[HARVEST]->setColumns(cols);
    }
    prevEndTime_ = -9999.0f;
    lastAddedTime_ = -9999.0f;
    requestRefresh();
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
        case HARVEST:
            cols = { { "Time", GraphTable::Time }, { "MGU-K (kJ)", GraphTable::Fixed1 } };
            if (mguhVisible_) cols.append(GraphTable::Column{ "MGU-H (kJ)", GraphTable::Fixed1 });
            break;
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
    const tnr::GraphSection sections[SECTIONS] = { tnr::GraphSection::PowerSplit,
        tnr::GraphSection::PowerHarvest, tnr::GraphSection::PowerStore, tnr::GraphSection::PowerFuel };
    ChartDomain domains[SECTIONS];
    QString cursorModeKey;
    for (int section = 0; section < SECTIONS; ++section) {
        const ChartWindow window = model_->effectiveChartWindow(sections[section]);
        const int selectedLap = model_->referenceLap(sections[section]);
        domains[section] = resolveChartDomain(d, window, selectedLap, endTime,
            model_->sectorBoundaries(), model_->chartPrimaryLap(endTime),
            model_->chartReferenceLap(window, selectedLap, endTime));
        chart_->setXRange(xId_[section], domains[section].lower, domains[section].upper);
        chart_->setAxisDistanceMode(xId_[section], domains[section].distance);
        chart_->syncAxisSessionMap(xId_[section],
            domains[section].distance ? domains[section].primary : nullptr,
            domains[section].currentTime);
        if (!domains[section].ticks.isEmpty()) chart_->setAxisLabelMap(
            xId_[section], domains[section].ticks, domains[section].tickLabels,
            chartWindowAccumulatesLaps(domains[section].window));
        else chart_->setAxisTimeTicker(xId_[section], "%m:%s");
        cursorModeKey += chartWindowKey(domains[section].window) + '|';
    }
    chart_->setCursorModeKey(cursorModeKey);
    chart_->setCursorSync(model_->cursorSync(), model_->secondaryVerticalCrosshair(),
                          model_->secondaryHorizontalCrosshair());
    QString runtimeKey = cursorModeKey;
    for (const ChartDomain& domain : domains)
        runtimeKey += QString("%1:%2:%3|")
            .arg(domain.primary ? domain.primary->lapNum : -1)
            .arg(domain.reference ? domain.reference->lapNum : -1)
            .arg(domain.window == ChartWindow::StintLaps ? qRound64(domain.lower * 1000.0) : 0);
    runtimeKey += QString("rev:%1").arg(model_->playbackDataRevision());
    bool allTime = true;
    for (const ChartDomain& domain : domains) allTime = allTime && chartWindowIsTime(domain.window);
    if (allTime) {
        const bool rebuild = dataModeKey_ != runtimeKey || endTime < prevEndTime_ ||
                             std::abs(endTime - prevEndTime_) > 1.0f;
        if (rebuild) {
            for (int id : { splitIceId_, splitMgukId_, harvKId_, harvHId_, storeId_, fuelId_,
                            splitIceRefId_, splitMgukRefId_, harvKRefId_, harvHRefId_, storeRefId_, fuelRefId_ })
                chart_->clear(id);
            lastAddedTime_ = domains[0].lower;
            for (int section = 1; section < SECTIONS; ++section)
                lastAddedTime_ = qMin(lastAddedTime_, float(domains[section].lower));
            dataModeKey_ = runtimeKey;
        }
        auto start = std::lower_bound(d.stsBuf.begin(), d.stsBuf.end(), lastAddedTime_ + 0.0001f,
            [](const StsSample& sample, float value) { return sample.t < value; });
        for (auto it = start; it != d.stsBuf.end(); ++it) {
            if (it->t > endTime) break;
            chart_->appendPoint(splitIceId_, it->t, it->ice_kw);
            chart_->appendPoint(splitMgukId_, it->t, it->mguk_kw);
            chart_->appendPoint(harvKId_, it->t, it->mguk_harvest_j / 1000.0f);
            if (mguhVisible_) chart_->appendPoint(harvHId_, it->t, it->mguh_harvest_j / 1000.0f);
            chart_->appendPoint(storeId_, it->t, it->ers);
            chart_->appendPoint(fuelId_, it->t, it->fuel_kg);
            lastAddedTime_ = it->t;
        }
        for (int id : { splitIceId_, splitMgukId_ }) chart_->trimBefore(id, domains[SPLIT].lower);
        for (int id : { harvKId_, harvHId_ }) chart_->trimBefore(id, domains[HARVEST].lower);
        chart_->trimBefore(storeId_, domains[STORE].lower); chart_->trimBefore(fuelId_, domains[FUEL].lower);
        for (int id : { splitIceRefId_, splitMgukRefId_, harvKRefId_, harvHRefId_, storeRefId_, fuelRefId_ })
            chart_->setSeriesVisible(id, false);
    }
    bool uniformNonTime = !allTime;
    for (int section = 1; section < SECTIONS && uniformNonTime; ++section)
        uniformNonTime = domains[section].window == domains[0].window &&
                         domains[section].primary == domains[0].primary &&
                         domains[section].reference == domains[0].reference &&
                         domains[section].lower == domains[0].lower;
    const bool nonTimeRebuild = !allTime && (dataModeKey_ != runtimeKey || endTime < prevEndTime_ ||
                                             std::abs(endTime - prevEndTime_) > 1.0f || !uniformNonTime);
    QVector<StsSample> samples[SECTIONS];
    if (nonTimeRebuild)
        for (int section = 0; section < SECTIONS; ++section) samples[section] = chartStatusSamples(d, domains[section]);
    auto fill = [&](int section, int primaryId, int referenceId, auto value, bool enabled = true) {
        if (!nonTimeRebuild) return;
        const ChartDomain& domain = domains[section];
        QVector<double> x, y, rx, ry;
        for (const StsSample& sample : samples[section]) {
            const double key = domain.distance ? d.distanceAtTime(domain.primary, sample.t) : sample.t;
            if (!qIsFinite(key)) continue;
            x.push_back(key); y.push_back(value(sample));
        }
        if (domain.comparison && domain.reference) for (const StsSample& sample : domain.reference->sts) {
            const double key = projectReferenceTime(d, domain, sample.t);
            if (qIsFinite(key)) { rx.push_back(key); ry.push_back(value(sample)); }
        }
        chart_->setSeriesData(primaryId, x, y); chart_->setSeriesData(referenceId, rx, ry);
        chart_->setSeriesVisible(referenceId, enabled && domain.comparison && chart_->seriesVisible(primaryId));
    };
    fill(SPLIT, splitIceId_, splitIceRefId_, [](const StsSample& s) { return double(s.ice_kw); });
    fill(SPLIT, splitMgukId_, splitMgukRefId_, [](const StsSample& s) { return double(s.mguk_kw); });
    fill(HARVEST, harvKId_, harvKRefId_, [](const StsSample& s) { return double(s.mguk_harvest_j / 1000.0f); });
    fill(HARVEST, harvHId_, harvHRefId_, [](const StsSample& s) { return double(s.mguh_harvest_j / 1000.0f); }, mguhVisible_);
    fill(STORE, storeId_, storeRefId_, [](const StsSample& s) { return double(s.ers); });
    fill(FUEL, fuelId_, fuelRefId_, [](const StsSample& s) { return double(s.fuel_kg); });
    if (nonTimeRebuild) {
        lastAddedTime_ = samples[SPLIT].isEmpty() ? float(domains[SPLIT].lower)
                                                  : samples[SPLIT].last().t;
        dataModeKey_ = runtimeKey;
    } else if (!allTime && uniformNonTime) {
        const ChartDomain& domain = domains[SPLIT];
        const QVector<StsSample>& source = domain.distance && domain.primary
            ? domain.primary->sts : d.stsBuf;
        auto start = std::lower_bound(source.begin(), source.end(), lastAddedTime_ + 0.0001f,
            [](const StsSample& sample, float value) { return sample.t < value; });
        for (auto it = start; it != source.end(); ++it) {
            if (it->t > endTime) break;
            if (it->t < domain.lower) continue;
            const double key = domain.distance ? d.distanceAtTime(domain.primary, it->t) : it->t;
            if (!qIsFinite(key)) continue;
            chart_->appendPoint(splitIceId_, key, it->ice_kw);
            chart_->appendPoint(splitMgukId_, key, it->mguk_kw);
            chart_->appendPoint(harvKId_, key, it->mguk_harvest_j / 1000.0f);
            if (mguhVisible_) chart_->appendPoint(harvHId_, key, it->mguh_harvest_j / 1000.0f);
            chart_->appendPoint(storeId_, key, it->ers);
            chart_->appendPoint(fuelId_, key, it->fuel_kg);
            lastAddedTime_ = it->t;
        }
    }
    chart_->fitAxisToVisibleSeries(harvYId_,
        { harvKId_, harvHId_, harvKRefId_, harvHRefId_ }, 0.0, harvestFixedMax_,
        model_->dynamicYAxis(tnr::GraphSection::PowerHarvest), true);

    // Feed any table-mode sections from the same window (newest sample on top).
    if (tableMode_[SPLIT] || tableMode_[HARVEST] || tableMode_[STORE] || tableMode_[FUEL]) {
        GraphTable* tSplit   = (tableMode_[SPLIT]   && visible_[SPLIT])   ? table_[SPLIT]   : nullptr;
        GraphTable* tHarvest = (tableMode_[HARVEST] && visible_[HARVEST]) ? table_[HARVEST] : nullptr;
        GraphTable* tStore   = (tableMode_[STORE]   && visible_[STORE])   ? table_[STORE]   : nullptr;
        GraphTable* tFuel    = (tableMode_[FUEL]    && visible_[FUEL])    ? table_[FUEL]    : nullptr;
        auto feed = [&](GraphTable* table, int section, auto add) {
            if (!table) return;
            const ChartDomain& domain = domains[section];
            table->setDistanceMode(domain.distance);
            table->beginRebuild(domain.lower, domain.upper,
                                chartWindowAccumulatesLaps(domain.window));
            const QVector<StsSample>& source = domain.distance && domain.primary
                ? domain.primary->sts : d.stsBuf;
            for (int i = source.size() - 1; i >= 0 && !table->full(); --i) {
                const StsSample& sample = source[i];
                if (sample.t > domain.currentTime) continue;
                const double coordinate = domain.distance
                    ? d.distanceAtTime(domain.primary, sample.t) : sample.t;
                if (!qIsFinite(coordinate) || coordinate < domain.lower || coordinate > domain.upper) continue;
                add(table, coordinate, sample);
            }
            table->endRebuild();
        };
        feed(tSplit, SPLIT, [](GraphTable* table, double x, const StsSample& s) {
            table->addRow(x, s.ice_kw, s.mguk_kw);
        });
        feed(tHarvest, HARVEST, [this](GraphTable* table, double x, const StsSample& s) {
            if (mguhVisible_) table->addRow(x, s.mguk_harvest_j / 1000.0f, s.mguh_harvest_j / 1000.0f);
            else table->addRow(x, s.mguk_harvest_j / 1000.0f);
        });
        feed(tStore, STORE, [](GraphTable* table, double x, const StsSample& s) {
            table->addRow(x, s.ers);
        });
        feed(tFuel, FUEL, [](GraphTable* table, double x, const StsSample& s) {
            table->addRow(x, s.fuel_kg);
        });
    }

    // ONE replot renders all four panels — but skip it when the chart is hidden
    // (every section in table mode), so nothing renders off-screen. rebuildLayout()
    // re-shows and requestRefresh()es on the way back to chart mode, so it repaints then.
    if (chart_->isVisible()) chart_->requestReplot();
    prevEndTime_ = endTime;
}
