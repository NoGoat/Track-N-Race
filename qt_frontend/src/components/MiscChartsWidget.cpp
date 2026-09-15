#include "MiscChartsWidget.h"
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
#include <limits>

namespace {
const QColor C_LAT("#F0A500"),   C_LONG("#5794F2");
const QColor C_FRONT("#73BF69"), C_REAR("#B877DB");
constexpr tnr::GraphSection kSections[] = {tnr::GraphSection::MiscGForce,
    tnr::GraphSection::MiscRideHeight, tnr::GraphSection::MiscGLateral,
    tnr::GraphSection::MiscGLongitudinal, tnr::GraphSection::MiscRideFront,
    tnr::GraphSection::MiscRideRear};
}

MiscChartsWidget::MiscChartsWidget(QWidget* parent)
    : QWidget(parent)
{
    outer_ = new QGridLayout(this);
    outer_->setContentsMargins(0, 0, 0, 0);
    outer_->setSpacing(ChartView::PanelGap);   // match the chart's inter-panel gap so
                                               // overlaid tables align with chart cells

    chart_ = new ChartView;
    const char* titles[] = {"G-FORCE", "RIDE HEIGHT", "LATERAL G-FORCE",
                            "LONGITUDINAL G-FORCE", "FRONT RIDE HEIGHT", "REAR RIDE HEIGHT"};
    for (int section = 0; section < SECTIONS; ++section) {
        if (section) chart_->addPanel();
        const bool motion = section == GFORCE || section == LATERAL || section == LONGITUDINAL;
        xId_[section] = chart_->addAxis(
            {ChartView::Side::Bottom, 0.0, windowS_, QColor(), true, 'f', 1, true}, section);
        const int yAxis = chart_->addAxis(
            {ChartView::Side::Left, motion ? -6.0 : 0.0, motion ? 6.0 : 100.0,
             QColor(), true, 'f', 0}, section);
        chart_->setAxisTimeTicker(xId_[section], "%m:%s");
        chart_->setAxisNumberSuffix(yAxis, 1.0, motion ? " G" : " mm");
        chart_->setPanelTitle(section, titles[section]);
        chart_->setPanelLegendVisible(section, true);
        const int first = section == LONGITUDINAL || section == REAR ? 1 : 0;
        const int count = section <= RIDEHEIGHT ? 2 : 1;
        for (int i = 0; i < count; ++i) {
            const int component = first + i;
            const QColor color = motion ? (component ? C_LONG : C_LAT) : (component ? C_REAR : C_FRONT);
            const QString label = motion ? (component ? "Longitudinal" : "Lateral")
                                         : (component ? "Rear" : "Front");
            primaryIds_[section][i] = chart_->addSeries(
                {label, color, 2.0, xId_[section], yAxis, motion ? " G" : " mm", motion ? 2 : 1});
            QColor reference = color; reference.setAlpha(105);
            referenceIds_[section][i] = chart_->addSeries(
                {"", reference, 1.2, xId_[section], yAxis, motion ? " G" : " mm", motion ? 2 : 1});
            chart_->setSeriesVisible(referenceIds_[section][i], false);
            chart_->linkSeriesVisibility(primaryIds_[section][i], referenceIds_[section][i]);
        }
    }

    chart_->setHoverReadout(true);
    rebuildLayout();

    // The shared presentation scheduler coalesces refreshes at the chart FPS cap.
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
    connect(m, &SessionModel::chartConfigurationChanged, this, &MiscChartsWidget::requestRefresh);
    for (int s = 0; s < SECTIONS; ++s) chart_->bindPanelChartSettings(s, m, kSections[s]);
    requestRefresh();
}

void MiscChartsWidget::setPlaybackMode(bool on) { playback_ = on; requestRefresh(); }
void MiscChartsWidget::setCurrentTime(float t)  { currentTime_ = t; requestRefresh(); }
void MiscChartsWidget::setWindowSeconds(float s) { windowS_ = s; for (QString& key : dataModeKey_) key.clear(); requestRefresh(); }
void MiscChartsWidget::requestRefresh() {
    dirty_ = true;
    if (!isVisible()) return;
    PresentationScheduler::instance().request(this, [this] {
        if (dirty_ && isVisible()) { dirty_ = false; refresh(); }
    }, PresentationScheduler::Policy::Chart);
}

void MiscChartsWidget::setSectionVisible(int section, bool on) {
    if (section < 0 || section >= SECTIONS) return;
    if (visible_[section] == on) return;
    visible_[section] = on;
    dataModeKey_[section].clear();
    rebuildLayout();
    requestRefresh();
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
        case LATERAL: cols = {{"Time", GraphTable::Time}, {"Lateral (G)", GraphTable::Fixed2}}; break;
        case LONGITUDINAL: cols = {{"Time", GraphTable::Time}, {"Longitudinal (G)", GraphTable::Fixed2}}; break;
        case FRONT: cols = {{"Time", GraphTable::Time}, {"Front (mm)", GraphTable::Fixed1}}; break;
        case REAR: cols = {{"Time", GraphTable::Time}, {"Rear (mm)", GraphTable::Fixed1}}; break;
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
    for (int s : {GFORCE, LATERAL, LONGITUDINAL, RIDEHEIGHT, FRONT, REAR})
        if (visible_[s]) rows.append(QVector<int>{s});

    tnr::layoutSectionGrid(outer_, chart_, rows, SECTIONS, tableMode_, table_,
                           [this](int s) { ensureTable(s); });
}

float MiscChartsWidget::currentTime() const {
    if (playback_) return currentTime_;
    return model_ ? model_->data().latestTime : 0.0f;
}

void MiscChartsWidget::refresh() {
    if (!model_ || !chart_) return;
    const SessionData& data = model_->data();
    const float endTime = currentTime();
    QString cursorKey;
    for (int section = 0; section < SECTIONS; ++section) {
        if (!visible_[section]) continue;
        const ChartWindow window = model_->effectiveChartWindow(kSections[section]);
        const int selected = model_->referenceLap(kSections[section]);
        const ChartDomain domain = resolveChartDomain(data, window, selected, endTime,
            model_->sectorBoundaries(), model_->chartPrimaryLap(endTime),
            model_->chartReferenceLap(window, selected, endTime));
        chart_->setXRange(xId_[section], domain.lower, domain.upper);
        chart_->setAxisDistanceMode(xId_[section], domain.distance);
        chart_->syncAxisSessionMap(xId_[section], domain.distance ? domain.primary : nullptr,
                                  domain.currentTime);
        if (!domain.ticks.isEmpty()) chart_->setAxisLabelMap(xId_[section], domain.ticks,
            domain.tickLabels, chartWindowAccumulatesLaps(window));
        else chart_->setAxisTimeTicker(xId_[section], "%m:%s");
        cursorKey += chartWindowKey(window) + '|';
        const QString runtimeKey = chartWindowKey(window) + QString("|%1:%2:%3:%4")
            .arg(domain.primary ? domain.primary->lapNum : -1)
            .arg(domain.reference ? domain.reference->lapNum : -1)
            .arg(window == ChartWindow::StintLaps ? qRound64(domain.lower * 1000.0) : 0)
            .arg(model_->playbackDataRevision());
        const bool rebuild = dataModeKey_[section] != runtimeKey ||
            endTime < previousTime_[section] || std::abs(endTime - previousTime_[section]) > 1.0f;
        const int count = section <= RIDEHEIGHT ? 2 : 1;
        const int first = section == LONGITUDINAL || section == REAR ? 1 : 0;
        const auto coordinate = [&](float time) {
            return domain.distance ? data.distanceAtTime(domain.primary, time) : double(time);
        };
        // Each active panel keeps its own cursor and domain. Split charts can
        // choose different windows without rescanning unchanged history each tick.
        auto feed = [&](const auto& live, auto member, auto value) {
            const auto& samples = domain.distance && domain.primary ? domain.primary->*member : live;
            QVector<double> xValues, referenceX;
            QVector<double> yValues[2], referenceY[2];
            if (rebuild) {
                lastAddedTime_[section] = -std::numeric_limits<float>::infinity();
            }
            const float nextTime = lastAddedTime_[section] + 0.0001f;
            const float start = domain.distance ? nextTime : qMax(float(domain.lower), nextTime);
            auto begin = std::lower_bound(samples.begin(), samples.end(), start,
                [](const auto& sample, float time) { return sample.t < time; });
            for (auto it = begin; it != samples.end(); ++it) {
                if (it->t > domain.currentTime) break;
                const double x = coordinate(it->t);
                if (!qIsFinite(x) || x < domain.lower || x > domain.upper) continue;
                lastAddedTime_[section] = it->t;
                if (rebuild) xValues.append(x);
                for (int i = 0; i < count; ++i) {
                    if (rebuild) yValues[i].append(value(*it, first + i));
                    else chart_->appendPoint(primaryIds_[section][i], x, value(*it, first + i));
                }
            }
            if (rebuild && domain.comparison && domain.reference) {
                for (const auto& sample : domain.reference->*member) {
                    const double x = projectReferenceTime(data, domain, sample.t);
                    if (!qIsFinite(x)) continue;
                    referenceX.append(x);
                    for (int i = 0; i < count; ++i)
                        referenceY[i].append(value(sample, first + i));
                }
            }
            for (int i = 0; i < count; ++i) {
                if (rebuild) {
                    chart_->setSeriesData(primaryIds_[section][i], xValues, yValues[i]);
                    chart_->setSeriesData(referenceIds_[section][i], referenceX, referenceY[i]);
                }
                chart_->trimBefore(primaryIds_[section][i], domain.lower);
                chart_->setSeriesVisible(referenceIds_[section][i],
                    domain.comparison && chart_->seriesVisible(primaryIds_[section][i]));
            }
            if (tableMode_[section] && table_[section]) {
                GraphTable* table = table_[section];
                table->setDistanceMode(domain.distance);
                table->beginRebuild(domain.lower, domain.upper, chartWindowAccumulatesLaps(window));
                for (int n = samples.size() - 1; n >= 0 && !table->full(); --n) {
                    const auto& sample = samples[n];
                    if (sample.t > domain.currentTime) continue;
                    const double x = coordinate(sample.t);
                    if (!qIsFinite(x) || x < domain.lower || x > domain.upper) continue;
                    if (count == 2) table->addRow(x, value(sample, 0), value(sample, 1));
                    else table->addRow(x, value(sample, first));
                }
                table->endRebuild();
            }
        };
        if (section == GFORCE || section == LATERAL || section == LONGITUDINAL) {
            feed(data.motionBuf, &LapBlock::motion,
                [](const MotionSample& sample, int component) {
                    return component ? sample.g_long : sample.g_lat;
                });
        } else {
            feed(data.motionExBuf, &LapBlock::motionEx,
                [](const MotionExSample& sample, int component) {
                    return component ? sample.rear_aero : sample.front_aero;
                });
        }
        dataModeKey_[section] = runtimeKey;
        previousTime_[section] = endTime;
    }
    chart_->setCursorModeKey(cursorKey);
    chart_->setCursorSync(model_->cursorSync(), model_->secondaryVerticalCrosshair(),
                          model_->secondaryHorizontalCrosshair());
    if (chart_->isVisible()) chart_->requestReplot();
}
