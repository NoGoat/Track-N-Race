#include "TyreChartsWidget.h"
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
const QColor C_FL("#e10600");
const QColor C_FR("#4488ff");
const QColor C_RL("#37872D");
const QColor C_RR("#ffd700");
const QColor kWheelColors[4] = { C_FL, C_FR, C_RL, C_RR };
const char*  kWheelNames[4]  = { "FL", "FR", "RL", "RR" };
}

TyreChartsWidget::TyreChartsWidget(bool grid, QWidget* parent)
    : QWidget(parent), grid_(grid)
{
    outer_ = new QGridLayout(this);
    outer_->setContentsMargins(0, 0, 0, 0);
    outer_->setSpacing(ChartView::PanelGap);   // match the chart's inter-panel gap so
                                               // overlaid tables align with chart cells

    // All four sections are panels of ONE ChartView, so they render in a single
    // QRhi render target / repaint rather than four separate widgets. Each
    // panel carries its own in-plot title and FL/FR/RL/RR colour-key legend.
    // Table-mode sections render as GraphTables overlaid in the same grid cell (see
    // rebuildLayout).
    chart_ = new ChartView;

    auto addSection = [&](int sec, const QString& title, double yMin, double yMax,
                          const QString& unit) {
        if (sec > 0) chart_->addPanel();   // panel 0 already exists in the ChartView
        xId_[sec] = chart_->addAxis(
            { ChartView::Side::Bottom, 0.0, windowS_, QColor(), true, 'f', 0, true, 60 }, sec);
        yId_[sec] = chart_->addAxis(
            { ChartView::Side::Left, yMin, yMax, QColor(), true, 'f', 0 }, sec);
        chart_->setAxisTimeTicker(xId_[sec], "%m:%s");
        // Build the header (title + colour key) before the series, so the series
        // register in this panel's key legend rather than the default one.
        chart_->setPanelTitle(sec, title);
        chart_->setPanelLegendVisible(sec, true);
        for (int w = 0; w < 4; ++w) {
            seriesIds_[sec][w] = chart_->addSeries({
                kWheelNames[w], kWheelColors[w], 1.5, xId_[sec], yId_[sec], unit, 0
            });
            QColor muted = kWheelColors[w]; muted.setAlpha(105);
            referenceIds_[sec][w] = chart_->addSeries({ "", muted, 1.1, xId_[sec], yId_[sec], unit, 0 });
            chart_->setSeriesVisible(referenceIds_[sec][w], false);
            chart_->linkSeriesVisibility(seriesIds_[sec][w], referenceIds_[sec][w]);
        }
    };

    addSection(SURF,  "SURFACE TEMP", 0, 125,  "°C");
    addSection(INNER, "INNER TEMP",   0, 125,  "°C");
    addSection(BRAKE, "BRAKE TEMP",   0, 1250, "°C");
    addSection(WEAR,  lifeMode_ ? "TYRE LIFE" : "TYRE WEAR", 0, 100, "%");

    // 2×2 grid for the fullscreen Tyres view, 1×4 row for the Overview strip.
    rebuildLayout();

    // Enable hover once every panel/axis exists (crosshairs are created per panel).
    chart_->setHoverReadout(true);

    // Zero-delay single-shot armed from requestRefresh(): coalesces to one rebuild
    // per event-loop pass (one per arriving packet, 20..60 Hz), no fixed rate cap.
}

void TyreChartsWidget::showEvent(QShowEvent* e) {
    QWidget::showEvent(e);
    requestRefresh();
}

void TyreChartsWidget::setModel(SessionModel* m) {
    model_ = m;
    if (!m) return;
    connect(m, &SessionModel::tyreAppended, this, &TyreChartsWidget::requestRefresh);
    connect(m, &SessionModel::wasReset,     this, &TyreChartsWidget::requestRefresh);
    connect(m, &SessionModel::chartConfigurationChanged, this, &TyreChartsWidget::requestRefresh);
    const tnr::GraphSection sections[SECTIONS] = {
        grid_ ? tnr::GraphSection::TyreSurface : tnr::GraphSection::OverviewTyreSurface,
        grid_ ? tnr::GraphSection::TyreInner : tnr::GraphSection::OverviewTyreInner,
        grid_ ? tnr::GraphSection::TyreBrake : tnr::GraphSection::OverviewTyreBrake,
        grid_ ? tnr::GraphSection::TyreWear : tnr::GraphSection::OverviewTyreWear
    };
    for (int i = 0; i < SECTIONS; ++i) chart_->bindPanelChartSettings(i, m, sections[i]);
    requestRefresh();
}

void TyreChartsWidget::setPlaybackMode(bool on) { playback_ = on; requestRefresh(); }
void TyreChartsWidget::setCurrentTime(float t)  { currentTime_ = t; requestRefresh(); }
void TyreChartsWidget::setWindowSeconds(float s) { windowS_ = s; prevEndTime_ = -9999.0f; requestRefresh(); }
void TyreChartsWidget::requestRefresh() {
    dirty_ = true;
    if (!isVisible()) return;
    PresentationScheduler::instance().request(this, [this] {
        if (dirty_ && isVisible()) { dirty_ = false; refresh(); }
    }, PresentationScheduler::Policy::Chart);
}

void TyreChartsWidget::setTyreLifeMode(bool life) {
    if (lifeMode_ == life) return;
    lifeMode_ = life;
    if (chart_) chart_->setPanelTitle(WEAR, life ? "TYRE LIFE" : "TYRE WEAR");
    prevEndTime_ = -9999.0f;   // force a full clear + re-append with the new mapping
    requestRefresh();
}

void TyreChartsWidget::setChartSectionVisible(int i, bool on)
{
    if (i < 0 || i >= SECTIONS || !chart_) return;
    if (visible_[i] == on) return;
    visible_[i] = on;
    rebuildLayout();
}

void TyreChartsWidget::setSectionViewMode(int section, bool table)
{
    if (section < 0 || section >= SECTIONS) return;
    if (tableMode_[section] == table) return;
    tableMode_[section] = table;
    rebuildLayout();
    requestRefresh();   // populate the freshly-shown table immediately
}

void TyreChartsWidget::ensureTable(int section)
{
    if (table_[section]) return;
    const char* unit = (section == WEAR) ? " (%)" : " (°C)";
    const QVector<GraphTable::Column> cols = {
        { "Time", GraphTable::Time },
        { QString("FL%1").arg(unit), GraphTable::Fixed0 },
        { QString("FR%1").arg(unit), GraphTable::Fixed0 },
        { QString("RL%1").arg(unit), GraphTable::Fixed0 },
        { QString("RR%1").arg(unit), GraphTable::Fixed0 } };
    table_[section] = new GraphTable(cols, this);
    table_[section]->setVisible(false);
}

void TyreChartsWidget::rebuildLayout()
{
    if (!chart_ || !outer_) return;
    // 2×2 grid (fullscreen) packs visible sections two-per-row; 1×4 (Overview strip)
    // is a single row. Hidden sections drop out and the rest reflow. Both chart- and
    // table-mode sections keep these positions; a table replaces its chart in place
    // (see tnr::layoutSectionGrid).
    QVector<int> vis;
    for (int s = 0; s < SECTIONS; ++s) if (visible_[s]) vis.append(s);

    const int perRow = grid_ ? 2 : 4;
    QVector<QVector<int>> rows;
    for (int i = 0; i < vis.size(); i += perRow)
        rows.append(vis.mid(i, perRow));

    tnr::layoutSectionGrid(outer_, chart_, rows, SECTIONS, tableMode_, table_,
                           [this](int s) { ensureTable(s); });
}

float TyreChartsWidget::currentTime() const {
    if (playback_) return currentTime_;
    return model_ ? model_->data().latestTime : 0.0f;
}

void TyreChartsWidget::refresh() {
    if (!model_ || !chart_) return;

    const SessionData& d = model_->data();
    const float endTime = currentTime();
    const auto wv = [this](float w) { return lifeMode_ ? 100.0f - w : w; };
    const tnr::GraphSection sections[SECTIONS] = {
        grid_ ? tnr::GraphSection::TyreSurface : tnr::GraphSection::OverviewTyreSurface,
        grid_ ? tnr::GraphSection::TyreInner : tnr::GraphSection::OverviewTyreInner,
        grid_ ? tnr::GraphSection::TyreBrake : tnr::GraphSection::OverviewTyreBrake,
        grid_ ? tnr::GraphSection::TyreWear : tnr::GraphSection::OverviewTyreWear
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

    auto tyreValue = [](int section, int wheel, const TyreSample& sample) {
        const float* values = nullptr;
        const float surface[] = { sample.surfFl, sample.surfFr, sample.surfRl, sample.surfRr };
        const float inner[] = { sample.innerFl, sample.innerFr, sample.innerRl, sample.innerRr };
        const float brake[] = { sample.brakeFl, sample.brakeFr, sample.brakeRl, sample.brakeRr };
        values = section == SURF ? surface : section == INNER ? inner : brake;
        return double(values[wheel]);
    };
    bool allTime = true;
    for (const ChartDomain& domain : domains) allTime = allTime && chartWindowIsTime(domain.window);
    if (allTime) {
        const bool rebuild = dataModeKey_ != runtimeKey || endTime < prevEndTime_ ||
                             std::abs(endTime - prevEndTime_) > 1.0f;
        if (rebuild) {
            for (int section = 0; section < SECTIONS; ++section)
                for (int wheel = 0; wheel < 4; ++wheel) {
                    chart_->clear(seriesIds_[section][wheel]);
                    chart_->clear(referenceIds_[section][wheel]);
                }
            lastAddedTime_ = qMin(domains[SURF].lower,
                qMin(domains[INNER].lower, domains[BRAKE].lower));
            lastAddedDamageTime_ = domains[WEAR].lower;
            dataModeKey_ = runtimeKey;
        }
        auto tyreStart = std::lower_bound(d.tyreBuf.begin(), d.tyreBuf.end(), lastAddedTime_ + 0.0001f,
            [](const TyreSample& sample, float value) { return sample.t < value; });
        for (auto it = tyreStart; it != d.tyreBuf.end(); ++it) {
            if (it->t > endTime) break;
            for (int section = SURF; section <= BRAKE; ++section)
                for (int wheel = 0; wheel < 4; ++wheel)
                    chart_->appendPoint(seriesIds_[section][wheel], it->t,
                                        tyreValue(section, wheel, *it));
            lastAddedTime_ = it->t;
        }
        auto damageStart = std::lower_bound(d.damageBuf.begin(), d.damageBuf.end(), lastAddedDamageTime_ + 0.0001f,
            [](const DamageSample& sample, float value) { return sample.t < value; });
        for (auto it = damageStart; it != d.damageBuf.end(); ++it) {
            if (it->t > endTime) break;
            const float values[] = { it->wearFl, it->wearFr, it->wearRl, it->wearRr };
            for (int wheel = 0; wheel < 4; ++wheel)
                chart_->appendPoint(seriesIds_[WEAR][wheel], it->t, wv(values[wheel]));
            lastAddedDamageTime_ = it->t;
        }
        for (int section = 0; section < SECTIONS; ++section)
            for (int wheel = 0; wheel < 4; ++wheel) {
                chart_->trimBefore(seriesIds_[section][wheel], domains[section].lower);
                chart_->setSeriesVisible(referenceIds_[section][wheel], false);
            }
    }
    bool uniformNonTime = !allTime;
    for (int section = 1; section < SECTIONS && uniformNonTime; ++section)
        uniformNonTime = domains[section].window == domains[0].window &&
                         domains[section].primary == domains[0].primary &&
                         domains[section].reference == domains[0].reference &&
                         domains[section].lower == domains[0].lower;
    const bool nonTimeRebuild = !allTime && (dataModeKey_ != runtimeKey || endTime < prevEndTime_ ||
                                             std::abs(endTime - prevEndTime_) > 1.0f || !uniformNonTime);
    if (nonTimeRebuild) for (int section = SURF; section <= BRAKE; ++section) {
        const ChartDomain& domain = domains[section];
        const QVector<TyreSample> source = chartTyreSamples(d, domain);
        for (int wheel = 0; wheel < 4; ++wheel) {
            QVector<double> x, y, rx, ry;
            for (const TyreSample& sample : source) {
                const double key = domain.distance ? d.distanceAtTime(domain.primary, sample.t) : sample.t;
                if (!qIsFinite(key)) continue;
                x.push_back(key); y.push_back(tyreValue(section, wheel, sample));
            }
            if (domain.comparison && domain.reference) for (const TyreSample& sample : domain.reference->tyre) {
                const double key = projectReferenceTime(d, domain, sample.t);
                if (qIsFinite(key)) { rx.push_back(key); ry.push_back(tyreValue(section, wheel, sample)); }
            }
            chart_->setSeriesData(seriesIds_[section][wheel], x, y);
            chart_->setSeriesData(referenceIds_[section][wheel], rx, ry);
            chart_->setSeriesVisible(referenceIds_[section][wheel],
                domain.comparison && chart_->seriesVisible(seriesIds_[section][wheel]));
        }
    }
    if (nonTimeRebuild) {
        const ChartDomain& domain = domains[WEAR];
        const QVector<DamageSample> source = chartDamageSamples(d, domain);
        for (int wheel = 0; wheel < 4; ++wheel) {
            QVector<double> x, y, rx, ry;
            auto value = [&](const DamageSample& sample) {
                const float values[] = { sample.wearFl, sample.wearFr, sample.wearRl, sample.wearRr };
                return double(wv(values[wheel]));
            };
            for (const DamageSample& sample : source) {
                const double key = domain.distance ? d.distanceAtTime(domain.primary, sample.t) : sample.t;
                if (!qIsFinite(key)) continue;
                x.push_back(key); y.push_back(value(sample));
            }
            if (domain.comparison && domain.reference) for (const DamageSample& sample : domain.reference->damage) {
                const double key = projectReferenceTime(d, domain, sample.t);
                if (qIsFinite(key)) { rx.push_back(key); ry.push_back(value(sample)); }
            }
            chart_->setSeriesData(seriesIds_[WEAR][wheel], x, y);
            chart_->setSeriesData(referenceIds_[WEAR][wheel], rx, ry);
            chart_->setSeriesVisible(referenceIds_[WEAR][wheel],
                domain.comparison && chart_->seriesVisible(seriesIds_[WEAR][wheel]));
        }
    }
    if (nonTimeRebuild) {
        const QVector<TyreSample> initialTyre = chartTyreSamples(d, domains[SURF]);
        const QVector<DamageSample> initialDamage = chartDamageSamples(d, domains[WEAR]);
        lastAddedTime_ = initialTyre.isEmpty() ? float(domains[SURF].lower) : initialTyre.last().t;
        lastAddedDamageTime_ = initialDamage.isEmpty() ? float(domains[WEAR].lower) : initialDamage.last().t;
        dataModeKey_ = runtimeKey;
    } else if (!allTime && uniformNonTime) {
        const ChartDomain& domain = domains[SURF];
        const QVector<TyreSample>& source = domain.distance && domain.primary
            ? domain.primary->tyre : d.tyreBuf;
        auto tyreStart = std::lower_bound(source.begin(), source.end(), lastAddedTime_ + 0.0001f,
            [](const TyreSample& sample, float value) { return sample.t < value; });
        for (auto it = tyreStart; it != source.end(); ++it) {
            if (it->t > endTime) break;
            if (it->t < domain.lower) continue;
            const double key = domain.distance ? d.distanceAtTime(domain.primary, it->t) : it->t;
            if (!qIsFinite(key)) continue;
            for (int section = SURF; section <= BRAKE; ++section)
                for (int wheel = 0; wheel < 4; ++wheel)
                    chart_->appendPoint(seriesIds_[section][wheel], key,
                                        tyreValue(section, wheel, *it));
            lastAddedTime_ = it->t;
        }
        const ChartDomain& wearDomain = domains[WEAR];
        const QVector<DamageSample>& damage = wearDomain.distance && wearDomain.primary
            ? wearDomain.primary->damage : d.damageBuf;
        auto damageStart = std::lower_bound(damage.begin(), damage.end(), lastAddedDamageTime_ + 0.0001f,
            [](const DamageSample& sample, float value) { return sample.t < value; });
        for (auto it = damageStart; it != damage.end(); ++it) {
            if (it->t > endTime) break;
            if (it->t < wearDomain.lower) continue;
            const double key = wearDomain.distance ? d.distanceAtTime(wearDomain.primary, it->t) : it->t;
            if (!qIsFinite(key)) continue;
            const float values[] = { it->wearFl, it->wearFr, it->wearRl, it->wearRr };
            for (int wheel = 0; wheel < 4; ++wheel)
                chart_->appendPoint(seriesIds_[WEAR][wheel], key, wv(values[wheel]));
            lastAddedDamageTime_ = it->t;
        }
    }
    const double fixedMax[SECTIONS] = { 125.0, 125.0, 1250.0, 100.0 };
    for (int section = 0; section < SECTIONS; ++section) {
        QVector<int> ids;
        for (int wheel = 0; wheel < 4; ++wheel) ids << seriesIds_[section][wheel] << referenceIds_[section][wheel];
        chart_->fitAxisToVisibleSeries(yId_[section], ids, 0.0, fixedMax[section],
            model_->dynamicYAxis(sections[section]), section != WEAR);
    }

    // Feed any table-mode sections from the same window (newest sample on top).
    bool anyTable = false;
    for (int s = 0; s < SECTIONS; ++s)
        if (tableMode_[s] && visible_[s] && table_[s]) anyTable = true;
    if (anyTable) {
        for (int s = 0; s < SECTIONS; ++s)
            if (tableMode_[s] && visible_[s] && table_[s]) {
                table_[s]->setDistanceMode(domains[s].distance);
                table_[s]->beginRebuild(domains[s].lower, domains[s].upper,
                                        chartWindowAccumulatesLaps(domains[s].window));
            }
        auto feedTyre = [&](int section, auto values) {
            GraphTable* table = tableMode_[section] && visible_[section] ? table_[section] : nullptr;
            if (!table) return;
            const ChartDomain& domain = domains[section];
            const QVector<TyreSample>& source = domain.distance && domain.primary
                ? domain.primary->tyre : d.tyreBuf;
            for (int i = source.size() - 1; i >= 0 && !table->full(); --i) {
                const TyreSample& sample = source[i];
                if (sample.t > domain.currentTime) continue;
                const double coordinate = domain.distance
                    ? d.distanceAtTime(domain.primary, sample.t) : sample.t;
                if (!qIsFinite(coordinate) || coordinate < domain.lower || coordinate > domain.upper) continue;
                values(table, coordinate, sample);
            }
        };
        feedTyre(SURF, [](GraphTable* table, double x, const TyreSample& s) {
            table->addRow(x, s.surfFl, s.surfFr, s.surfRl, s.surfRr);
        });
        feedTyre(INNER, [](GraphTable* table, double x, const TyreSample& s) {
            table->addRow(x, s.innerFl, s.innerFr, s.innerRl, s.innerRr);
        });
        feedTyre(BRAKE, [](GraphTable* table, double x, const TyreSample& s) {
            table->addRow(x, s.brakeFl, s.brakeFr, s.brakeRl, s.brakeRr);
        });
        GraphTable* wearTable = tableMode_[WEAR] && visible_[WEAR] ? table_[WEAR] : nullptr;
        if (wearTable) {
            const ChartDomain& domain = domains[WEAR];
            const QVector<DamageSample>& source = domain.distance && domain.primary
                ? domain.primary->damage : d.damageBuf;
            for (int i = source.size() - 1; i >= 0 && !wearTable->full(); --i) {
                const DamageSample& sample = source[i];
                if (sample.t > domain.currentTime) continue;
                const double coordinate = domain.distance
                    ? d.distanceAtTime(domain.primary, sample.t) : sample.t;
                if (!qIsFinite(coordinate) || coordinate < domain.lower || coordinate > domain.upper) continue;
                wearTable->addRow(coordinate, wv(sample.wearFl), wv(sample.wearFr),
                                  wv(sample.wearRl), wv(sample.wearRr));
            }
        }
        for (int s = 0; s < SECTIONS; ++s)
            if (tableMode_[s] && visible_[s] && table_[s]) table_[s]->endRebuild();
    }

    // ONE replot renders all four panels — but skip it when the chart is hidden
    // (every section in table mode), so nothing renders off-screen. rebuildLayout()
    // re-shows and requestRefresh()es on the way back to chart mode, so it repaints then.
    if (chart_->isVisible()) chart_->requestReplot();
    prevEndTime_ = endTime;
}
