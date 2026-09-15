#include "InputChartsWidget.h"
#include "../PresentationScheduler.h"
#include "ChartView.h"
#include "GraphTable.h"
#include "../SessionModel.h"
#include "../ChartCoordinates.h"

#include <QColor>
#include <QGridLayout>
#include <QShowEvent>
#include <QtMath>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
const QColor C_GEAR("#5794F2");
const QColor C_ACCELERATOR("#37872D"), C_BRAKE("#C4162A");
const QColor C_STEER("#BF5FFF");
constexpr tnr::GraphSection kSections[] = {
    tnr::GraphSection::InputGear,
    tnr::GraphSection::InputThrottleBrake,
    tnr::GraphSection::InputSteering,
    tnr::GraphSection::InputThrottleBrakeOverlay,
    tnr::GraphSection::InputAccelerator,
    tnr::GraphSection::InputBrake,
};
}

InputChartsWidget::InputChartsWidget(QWidget* parent)
    : QWidget(parent)
{
    for (int section = 0; section < SECTIONS; ++section)
        for (int component = 0; component < 2; ++component) {
            primaryIds_[section][component] = -1;
            referenceIds_[section][component] = -1;
        }
    std::fill(lastAddedTime_, lastAddedTime_ + SECTIONS,
              -std::numeric_limits<float>::infinity());
    std::fill(previousTime_, previousTime_ + SECTIONS,
              -std::numeric_limits<float>::infinity());

    outer_ = new QGridLayout(this);
    outer_->setContentsMargins(0, 0, 0, 0);
    outer_->setSpacing(ChartView::PanelGap);
    chart_ = new ChartView;

    const char* titles[] = {
        "GEAR INDICATOR", "ACCELERATOR / BRAKE", "STEERING TELEMETRY   ( - Left / + Right )",
        "ACCELERATOR / BRAKE", "ACCELERATOR", "BRAKE"
    };
    for (int section = 0; section < SECTIONS; ++section) {
        if (section) chart_->addPanel();
        xId_[section] = chart_->addAxis(
            {ChartView::Side::Bottom, 0.0, windowS_, QColor(), true, 'f', 0, true}, section);
        const bool gear = section == GEAR;
        const bool signedValues = section == COMBINED || section == STEERING;
        const int yAxis = chart_->addAxis(
            {ChartView::Side::Left, gear ? 0.0 : signedValues ? -1.0 : 0.0,
             gear ? 9.0 : 1.0, QColor(), true, 'f', gear ? 0 : 2}, section);
        if (gear) {
            chart_->addBand({yAxis, 0.0, 2.5, QColor(196, 22, 42, 51)});
            chart_->addBand({yAxis, 2.5, 4.5, QColor(212, 173, 4, 51)});
            chart_->addBand({yAxis, 4.5, 6.5, QColor(0, 200, 83, 51)});
            chart_->addBand({yAxis, 6.5, 9.0, QColor(31, 96, 196, 51)});
        }
        chart_->setAxisTimeTicker(xId_[section], "%m:%s");
        chart_->setPanelTitle(section, titles[section]);
        chart_->setPanelLegendVisible(section, true);

        const int count = section == COMBINED || section == COMBINED2 ? 2 : 1;
        for (int component = 0; component < count; ++component) {
            const bool accelerator = section == ACCELERATOR ||
                                     ((section == COMBINED || section == COMBINED2) && component == 0);
            const bool brake = section == BRAKE ||
                               ((section == COMBINED || section == COMBINED2) && component == 1);
            const QColor color = gear ? C_GEAR : section == STEERING ? C_STEER
                                      : accelerator ? C_ACCELERATOR : C_BRAKE;
            const QString label = gear ? "Gear" : section == STEERING ? "Steering"
                                        : accelerator ? "Accelerator" : "Brake";
            const bool fill = section == COMBINED || section == ACCELERATOR || section == BRAKE;
            const bool step = gear;
            primaryIds_[section][component] = chart_->addSeries(
                {label, color, section == COMBINED2 ? 2.25 : 2.0, xId_[section], yAxis,
                 "", gear ? 0 : 2, false, fill, QColor(), step});
            QColor reference = color;
            reference.setAlpha(105);
            referenceIds_[section][component] = chart_->addSeries(
                {"", reference, 1.3, xId_[section], yAxis, "", gear ? 0 : 2,
                 false, false, QColor(), step});
            chart_->setSeriesVisible(referenceIds_[section][component], false);
            chart_->linkSeriesVisibility(primaryIds_[section][component],
                                         referenceIds_[section][component]);
        }
    }

    chart_->setHoverReadout(true);
    rebuildLayout();
}

void InputChartsWidget::showEvent(QShowEvent* e) {
    QWidget::showEvent(e);
    requestRefresh();
}

void InputChartsWidget::setModel(SessionModel* m) {
    model_ = m;
    if (!m) return;
    connect(m, &SessionModel::telemetryAppended, this, &InputChartsWidget::requestRefresh);
    connect(m, &SessionModel::wasReset, this, &InputChartsWidget::requestRefresh);
    connect(m, &SessionModel::chartConfigurationChanged, this, &InputChartsWidget::requestRefresh);
    for (int section = 0; section < SECTIONS; ++section)
        chart_->bindPanelChartSettings(section, m, kSections[section]);
    requestRefresh();
}

void InputChartsWidget::setPlaybackMode(bool on) { playback_ = on; requestRefresh(); }
void InputChartsWidget::setCurrentTime(float t) { currentTime_ = t; requestRefresh(); }
void InputChartsWidget::setWindowSeconds(float s) {
    windowS_ = s;
    for (QString& key : dataModeKey_) key.clear();
    requestRefresh();
}

void InputChartsWidget::requestRefresh() {
    dirty_ = true;
    if (!isVisible()) return;
    PresentationScheduler::instance().request(this, [this] {
        if (dirty_ && isVisible()) { dirty_ = false; refresh(); }
    }, PresentationScheduler::Policy::Chart);
}

void InputChartsWidget::setSectionVisible(int section, bool on) {
    if (section < 0 || section >= SECTIONS || visible_[section] == on) return;
    visible_[section] = on;
    dataModeKey_[section].clear();
    rebuildLayout();
    requestRefresh();
}

void InputChartsWidget::setSectionViewMode(int section, bool table) {
    if (section < 0 || section >= SECTIONS || tableMode_[section] == table) return;
    tableMode_[section] = table;
    rebuildLayout();
    requestRefresh();
}

void InputChartsWidget::setPageLayout(InputPageLayout layout) {
    if (pageLayout_ == layout) return;
    pageLayout_ = layout;
    rebuildLayout();
}

void InputChartsWidget::setPedalLayout(InputPedalLayout layout) {
    if (pedalLayout_ == layout) return;
    pedalLayout_ = layout;
    rebuildLayout();
}

void InputChartsWidget::setPedalVisibility(bool accelerator, bool brake) {
    if (showAccelerator_ == accelerator && showBrake_ == brake) return;
    showAccelerator_ = accelerator;
    showBrake_ = brake;
    for (int section : {COMBINED, COMBINED2}) {
        chart_->setSeriesVisible(primaryIds_[section][0], accelerator);
        chart_->setSeriesVisible(primaryIds_[section][1], brake);
        chart_->setSeriesVisible(referenceIds_[section][0], false);
        chart_->setSeriesVisible(referenceIds_[section][1], false);
        if (table_[section]) {
            QVector<GraphTable::Column> columns{{"Time", GraphTable::Time}};
            if (accelerator) columns.append({"Accelerator", GraphTable::Fixed2});
            if (brake) columns.append({"Brake", GraphTable::Fixed2});
            table_[section]->setColumns(columns);
        }
    }
    requestRefresh();
}

void InputChartsWidget::ensureTable(int section) {
    if (table_[section]) return;
    QVector<GraphTable::Column> columns{{"Time", GraphTable::Time}};
    switch (section) {
        case GEAR: columns.append({"Gear", GraphTable::Fixed0}); break;
        case COMBINED:
        case COMBINED2:
            if (showAccelerator_) columns.append({"Accelerator", GraphTable::Fixed2});
            if (showBrake_) columns.append({"Brake", GraphTable::Fixed2});
            break;
        case STEERING: columns.append({"Steering", GraphTable::Fixed2}); break;
        case ACCELERATOR: columns.append({"Accelerator", GraphTable::Fixed2}); break;
        case BRAKE: columns.append({"Brake", GraphTable::Fixed2}); break;
    }
    table_[section] = new GraphTable(columns, this);
    table_[section]->setVisible(false);
}

void InputChartsWidget::rebuildLayout() {
    if (!chart_ || !outer_) return;
    QVector<QVector<int>> rows;
    const int combined = pedalLayout_ == InputPedalLayout::Combined2 ? COMBINED2 : COMBINED;
    if (pageLayout_ == InputPageLayout::Vertical) {
        for (int section : pedalLayout_ == InputPedalLayout::Split
                ? QVector<int>{GEAR, ACCELERATOR, BRAKE, STEERING}
                : QVector<int>{GEAR, combined, STEERING})
            if (visible_[section]) rows.append(QVector<int>{section});
    } else if (pedalLayout_ == InputPedalLayout::Split) {
        QVector<int> pedals;
        if (visible_[ACCELERATOR]) pedals.append(ACCELERATOR);
        if (visible_[BRAKE]) pedals.append(BRAKE);
        if (!pedals.isEmpty()) rows.append(pedals);
        QVector<int> lower;
        if (visible_[GEAR]) lower.append(GEAR);
        if (visible_[STEERING]) lower.append(STEERING);
        if (!lower.isEmpty()) rows.append(lower);
    } else {
        QVector<int> upper;
        if (visible_[GEAR]) upper.append(GEAR);
        if (visible_[combined]) upper.append(combined);
        if (!upper.isEmpty()) rows.append(upper);
        if (visible_[STEERING]) rows.append(QVector<int>{STEERING});
    }
    tnr::layoutSectionGrid(outer_, chart_, rows, SECTIONS, tableMode_, table_,
                           [this](int section) { ensureTable(section); });
}

float InputChartsWidget::currentTime() const {
    return playback_ ? currentTime_ : model_ ? model_->data().latestTime : 0.0f;
}

void InputChartsWidget::refresh() {
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
        if (!domain.ticks.isEmpty())
            chart_->setAxisLabelMap(xId_[section], domain.ticks, domain.tickLabels,
                                    chartWindowAccumulatesLaps(window));
        else
            chart_->setAxisTimeTicker(xId_[section], "%m:%s");
        cursorKey += chartWindowKey(window) + '|';

        const QString runtimeKey = chartWindowKey(window) + QString("|%1:%2:%3:%4")
            .arg(domain.primary ? domain.primary->lapNum : -1)
            .arg(domain.reference ? domain.reference->lapNum : -1)
            .arg(window == ChartWindow::StintLaps ? qRound64(domain.lower * 1000.0) : 0)
            .arg(model_->playbackDataRevision());
        const bool rebuild = dataModeKey_[section] != runtimeKey ||
            endTime < previousTime_[section] || std::abs(endTime - previousTime_[section]) > 1.0f;
        const QVector<TelSample>& samples = domain.distance && domain.primary
            ? domain.primary->tel : data.telBuf;
        const int count = section == COMBINED || section == COMBINED2 ? 2 : 1;
        auto value = [section](const TelSample& sample, int component) -> double {
            switch (section) {
                case GEAR: return sample.gear;
                case STEERING: return sample.steering;
                case ACCELERATOR: return sample.throttle;
                case BRAKE: return sample.brake;
                case COMBINED: return component == 0 ? sample.throttle : -sample.brake;
                case COMBINED2: return component == 0 ? sample.throttle : sample.brake;
            }
            return 0;
        };
        auto coordinate = [&](float time) {
            return domain.distance ? data.distanceAtTime(domain.primary, time) : double(time);
        };

        QVector<double> xValues, referenceX;
        QVector<double> yValues[2], referenceY[2];
        if (rebuild) lastAddedTime_[section] = -std::numeric_limits<float>::infinity();
        const float nextTime = lastAddedTime_[section] + 0.0001f;
        const float start = domain.distance ? nextTime : qMax(float(domain.lower), nextTime);
        auto begin = std::lower_bound(samples.begin(), samples.end(), start,
            [](const TelSample& sample, float time) { return sample.t < time; });
        for (auto it = begin; it != samples.end(); ++it) {
            if (it->t > domain.currentTime) break;
            const double x = coordinate(it->t);
            if (!qIsFinite(x) || x < domain.lower || x > domain.upper) continue;
            lastAddedTime_[section] = it->t;
            if (rebuild) xValues.append(x);
            for (int component = 0; component < count; ++component) {
                if (rebuild) yValues[component].append(value(*it, component));
                else chart_->appendPoint(primaryIds_[section][component], x,
                                         value(*it, component));
            }
        }
        if (rebuild && domain.comparison && domain.reference) {
            for (const TelSample& sample : domain.reference->tel) {
                const double x = projectReferenceTime(data, domain, sample.t);
                if (!qIsFinite(x)) continue;
                referenceX.append(x);
                for (int component = 0; component < count; ++component)
                    referenceY[component].append(value(sample, component));
            }
        }
        for (int component = 0; component < count; ++component) {
            if (rebuild) {
                chart_->setSeriesData(primaryIds_[section][component], xValues,
                                      yValues[component]);
                chart_->setSeriesData(referenceIds_[section][component], referenceX,
                                      referenceY[component]);
            }
            chart_->trimBefore(primaryIds_[section][component], domain.lower);
            const bool configured = section != COMBINED && section != COMBINED2
                ? true : component == 0 ? showAccelerator_ : showBrake_;
            chart_->setSeriesVisible(referenceIds_[section][component], configured &&
                domain.comparison && chart_->seriesVisible(primaryIds_[section][component]));
        }

        if (tableMode_[section] && table_[section]) {
            GraphTable* table = table_[section];
            table->setDistanceMode(domain.distance);
            table->beginRebuild(domain.lower, domain.upper, chartWindowAccumulatesLaps(window));
            for (int i = samples.size() - 1; i >= 0 && !table->full(); --i) {
                const TelSample& sample = samples[i];
                if (sample.t > domain.currentTime) continue;
                const double x = coordinate(sample.t);
                if (!qIsFinite(x) || x < domain.lower || x > domain.upper) continue;
                if (section == COMBINED || section == COMBINED2) {
                    const double brakeValue = section == COMBINED ? sample.brake : value(sample, 1);
                    if (showAccelerator_ && showBrake_) table->addRow(x, value(sample, 0), brakeValue);
                    else if (showAccelerator_) table->addRow(x, value(sample, 0));
                    else if (showBrake_) table->addRow(x, brakeValue);
                } else {
                    table->addRow(x, value(sample, 0));
                }
            }
            table->endRebuild();
        }

        dataModeKey_[section] = runtimeKey;
        previousTime_[section] = endTime;
    }

    chart_->setCursorModeKey(cursorKey);
    chart_->setCursorSync(model_->cursorSync(), model_->secondaryVerticalCrosshair(),
                          model_->secondaryHorizontalCrosshair());
    if (chart_->isVisible()) chart_->requestReplot();
}
