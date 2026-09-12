#include "InputChartsWidget.h"
#include "../PresentationScheduler.h"
#include "ChartView.h"
#include "GraphTable.h"
#include "../SessionModel.h"
#include "../ChartCoordinates.h"

#include <QGridLayout>
#include <QColor>
#include <QShowEvent>
#include <QStringList>
#include <QLayoutItem>
#include <QtMath>
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

    // All three sections are panels of one ChartView — a single QRhi render target /
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
    chart_->setPanelLegendVisible(GEAR, true);
    gearId_ = chart_->addSeries(
        { "Gear", C_GEAR, 2.0, xId_[GEAR], axGear, "", 0, false, false, QColor(), true });
    QColor gearRef = C_GEAR; gearRef.setAlpha(105);
    gearRefId_ = chart_->addSeries({ "", gearRef, 1.3, xId_[GEAR], axGear, "", 0, false, false, QColor(), true });
    chart_->setSeriesVisible(gearRefId_, false);
    chart_->linkSeriesVisibility(gearId_, gearRefId_);

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
    QColor throttleRef = C_THROTTLE; throttleRef.setAlpha(105);
    QColor brakeRef = C_BRAKE; brakeRef.setAlpha(105);
    thRefId_ = chart_->addSeries({ "", throttleRef, 1.3, xId_[INPUTS], axPctIn, "", 2 });
    brRefId_ = chart_->addSeries({ "", brakeRef, 1.3, xId_[INPUTS], axPctIn, "", 2 });
    chart_->setSeriesVisible(thRefId_, false); chart_->setSeriesVisible(brRefId_, false);
    chart_->linkSeriesVisibility(thId_, thRefId_); chart_->linkSeriesVisibility(brId_, brRefId_);

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
    QColor steeringRef = C_STEER; steeringRef.setAlpha(105);
    stRefId_ = chart_->addSeries({ "", steeringRef, 1.3, xId_[STEERING], axPctSt, "", 2 });
    chart_->setSeriesVisible(stRefId_, false);
    chart_->linkSeriesVisibility(stId_, stRefId_);

    chart_->setHoverReadout(true);   // once all panels/axes exist (per-panel crosshairs)
    rebuildLayout();

    // Zero-delay single-shot armed from requestRefresh(): coalesces to one rebuild
    // per event-loop pass (one per arriving packet, 20..60 Hz), no fixed rate cap.
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
    connect(m, &SessionModel::chartConfigurationChanged, this, &InputChartsWidget::requestRefresh);
    chart_->bindPanelChartSettings(GEAR, m, tnr::GraphSection::InputGear);
    chart_->bindPanelChartSettings(INPUTS, m, tnr::GraphSection::InputThrottleBrake);
    chart_->bindPanelChartSettings(STEERING, m, tnr::GraphSection::InputSteering);
    requestRefresh();
}

void InputChartsWidget::setPlaybackMode(bool on) { playback_ = on; requestRefresh(); }
void InputChartsWidget::setCurrentTime(float t)  { currentTime_ = t; requestRefresh(); }
void InputChartsWidget::setWindowSeconds(float s) { windowS_ = s; prevEndTime_ = -9999.0f; requestRefresh(); }
void InputChartsWidget::requestRefresh() {
    dirty_ = true;
    if (!isVisible()) return;
    PresentationScheduler::instance().request(this, [this] {
        if (dirty_ && isVisible()) { dirty_ = false; refresh(); }
    }, PresentationScheduler::Policy::Chart);
}

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
    const tnr::GraphSection sections[SECTIONS] = {
        tnr::GraphSection::InputGear, tnr::GraphSection::InputThrottleBrake,
        tnr::GraphSection::InputSteering
    };
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
        if (!domains[section].ticks.isEmpty())
            chart_->setAxisLabelMap(xId_[section], domains[section].ticks, domains[section].tickLabels,
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
    const bool allTime = chartWindowIsTime(domains[GEAR].window) &&
                         chartWindowIsTime(domains[INPUTS].window) &&
                         chartWindowIsTime(domains[STEERING].window);
    if (allTime) {
        const bool rebuild = dataModeKey_ != runtimeKey || endTime < prevEndTime_ ||
                             std::abs(endTime - prevEndTime_) > 1.0f;
        if (rebuild) {
            for (int id : { gearId_, thId_, brId_, stId_, gearRefId_, thRefId_, brRefId_, stRefId_ })
                chart_->clear(id);
            lastAddedTime_ = qMin(domains[GEAR].lower,
                qMin(domains[INPUTS].lower, domains[STEERING].lower));
            dataModeKey_ = runtimeKey;
        }
        auto start = std::lower_bound(d.telBuf.begin(), d.telBuf.end(), lastAddedTime_ + 0.0001f,
            [](const TelSample& sample, float value) { return sample.t < value; });
        for (auto it = start; it != d.telBuf.end(); ++it) {
            if (it->t > endTime) break;
            chart_->appendPoint(gearId_, it->t, it->gear);
            chart_->appendPoint(thId_, it->t, it->throttle);
            chart_->appendPoint(brId_, it->t, -it->brake);
            chart_->appendPoint(stId_, it->t, it->steering);
            lastAddedTime_ = it->t;
        }
        chart_->trimBefore(gearId_, domains[GEAR].lower);
        chart_->trimBefore(thId_, domains[INPUTS].lower); chart_->trimBefore(brId_, domains[INPUTS].lower);
        chart_->trimBefore(stId_, domains[STEERING].lower);
        for (int id : { gearRefId_, thRefId_, brRefId_, stRefId_ }) chart_->setSeriesVisible(id, false);
    }

    bool uniformNonTime = !allTime;
    for (int section = 1; section < SECTIONS && uniformNonTime; ++section)
        uniformNonTime = domains[section].window == domains[0].window &&
                         domains[section].primary == domains[0].primary &&
                         domains[section].reference == domains[0].reference &&
                         domains[section].lower == domains[0].lower;
    const bool nonTimeRebuild = !allTime && (dataModeKey_ != runtimeKey || endTime < prevEndTime_ ||
                                             std::abs(endTime - prevEndTime_) > 1.0f || !uniformNonTime);
    QVector<TelSample> samples[SECTIONS];
    if (nonTimeRebuild)
        for (int section = 0; section < SECTIONS; ++section) samples[section] = chartTelSamples(d, domains[section]);

    auto fill = [&](int section, int primaryId, int referenceId, auto value) {
        if (!nonTimeRebuild) return;
        const ChartDomain& domain = domains[section];
        QVector<double> x, y, rx, ry;
        for (const TelSample& sample : samples[section]) {
            const double key = domain.distance ? d.distanceAtTime(domain.primary, sample.t) : sample.t;
            if (!qIsFinite(key)) continue;
            x.push_back(key); y.push_back(value(sample));
        }
        if (domain.comparison && domain.reference) {
            for (const TelSample& sample : domain.reference->tel) {
                const double key = projectReferenceTime(d, domain, sample.t);
                if (!qIsFinite(key)) continue;
                rx.push_back(key); ry.push_back(value(sample));
            }
        }
        chart_->setSeriesData(primaryId, x, y);
        chart_->setSeriesData(referenceId, rx, ry);
        chart_->setSeriesVisible(referenceId, domain.comparison && chart_->seriesVisible(primaryId));
    };
    fill(GEAR, gearId_, gearRefId_, [](const TelSample& s) { return double(s.gear); });
    fill(INPUTS, thId_, thRefId_, [](const TelSample& s) { return double(s.throttle); });
    fill(INPUTS, brId_, brRefId_, [](const TelSample& s) { return -double(s.brake); });
    fill(STEERING, stId_, stRefId_, [](const TelSample& s) { return double(s.steering); });
    if (nonTimeRebuild) {
        lastAddedTime_ = samples[GEAR].isEmpty() ? float(domains[GEAR].lower)
                                                 : samples[GEAR].last().t;
        dataModeKey_ = runtimeKey;
    } else if (!allTime && uniformNonTime) {
        const ChartDomain& domain = domains[GEAR];
        const QVector<TelSample>& source = domain.distance && domain.primary
            ? domain.primary->tel : d.telBuf;
        auto start = std::lower_bound(source.begin(), source.end(), lastAddedTime_ + 0.0001f,
            [](const TelSample& sample, float value) { return sample.t < value; });
        for (auto it = start; it != source.end(); ++it) {
            if (it->t > endTime) break;
            if (it->t < domain.lower) continue;
            const double key = domain.distance ? d.distanceAtTime(domain.primary, it->t) : it->t;
            if (!qIsFinite(key)) continue;
            chart_->appendPoint(gearId_, key, it->gear);
            chart_->appendPoint(thId_, key, it->throttle);
            chart_->appendPoint(brId_, key, -it->brake);
            chart_->appendPoint(stId_, key, it->steering);
            lastAddedTime_ = it->t;
        }
    }

    // Feed any table-mode sections from the same window (newest sample on top).
    const bool anyTable = tableMode_[GEAR] || tableMode_[INPUTS] || tableMode_[STEERING];
    if (anyTable) {
        GraphTable* tg = (tableMode_[GEAR]     && visible_[GEAR])     ? table_[GEAR]     : nullptr;
        GraphTable* ti = (tableMode_[INPUTS]   && visible_[INPUTS])   ? table_[INPUTS]   : nullptr;
        GraphTable* ts = (tableMode_[STEERING] && visible_[STEERING]) ? table_[STEERING] : nullptr;
        auto feed = [&](GraphTable* table, const ChartDomain& domain, auto add) {
            if (!table) return;
            table->setDistanceMode(domain.distance);
            table->beginRebuild(domain.lower, domain.upper,
                                chartWindowAccumulatesLaps(domain.window));
            const QVector<TelSample>& source = domain.distance && domain.primary
                ? domain.primary->tel : d.telBuf;
            for (int i = source.size() - 1; i >= 0 && !table->full(); --i) {
                const TelSample& sample = source[i];
                if (sample.t > domain.currentTime) continue;
                const double coordinate = domain.distance
                    ? d.distanceAtTime(domain.primary, sample.t) : sample.t;
                if (!qIsFinite(coordinate) || coordinate < domain.lower || coordinate > domain.upper) continue;
                add(table, coordinate, sample);
            }
            table->endRebuild();
        };
        feed(tg, domains[GEAR], [](GraphTable* table, double x, const TelSample& s) {
            table->addRow(x, s.gear);
        });
        feed(ti, domains[INPUTS], [](GraphTable* table, double x, const TelSample& s) {
            table->addRow(x, s.throttle, s.brake);
        });
        feed(ts, domains[STEERING], [](GraphTable* table, double x, const TelSample& s) {
            table->addRow(x, s.steering);
        });
    }

    // ONE replot renders all three panels — but skip it when the chart is hidden
    // (every section in table mode), so nothing renders off-screen. rebuildLayout()
    // re-shows and requestRefresh()es on the way back to chart mode, so it repaints then.
    if (chart_->isVisible()) chart_->requestReplot();
    prevEndTime_ = endTime;
}
