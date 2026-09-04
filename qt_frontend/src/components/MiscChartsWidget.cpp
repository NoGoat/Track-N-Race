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

    // Both sections are panels of one ChartView — a single QRhi render target
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
    QColor latRef = C_LAT; latRef.setAlpha(105); QColor longRef = C_LONG; longRef.setAlpha(105);
    latRefId_ = chart_->addSeries({ "", latRef, 1.2, xId_[GFORCE], axG, " G", 2 });
    longRefId_ = chart_->addSeries({ "", longRef, 1.2, xId_[GFORCE], axG, " G", 2 });

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
    QColor frontRef = C_FRONT; frontRef.setAlpha(105); QColor rearRef = C_REAR; rearRef.setAlpha(105);
    frontRefId_ = chart_->addSeries({ "", frontRef, 1.2, xId_[RIDEHEIGHT], axRH, " mm", 1 });
    rearRefId_ = chart_->addSeries({ "", rearRef, 1.2, xId_[RIDEHEIGHT], axRH, " mm", 1 });
    const int primary[] = { latId_, longId_, frontId_, rearId_ };
    const int refs[] = { latRefId_, longRefId_, frontRefId_, rearRefId_ };
    for (int i = 0; i < 4; ++i) { chart_->setSeriesVisible(refs[i], false); chart_->linkSeriesVisibility(primary[i], refs[i]); }

    chart_->setHoverReadout(true);
    rebuildLayout();

    // Zero-delay single-shot armed from requestRefresh(): coalesces to one rebuild
    // per event-loop pass (one per arriving packet, 20..60 Hz), no fixed rate cap.
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
    chart_->bindPanelChartSettings(GFORCE, m, tnr::GraphSection::MiscGForce);
    chart_->bindPanelChartSettings(RIDEHEIGHT, m, tnr::GraphSection::MiscRideHeight);
    requestRefresh();
}

void MiscChartsWidget::setPlaybackMode(bool on) { playback_ = on; requestRefresh(); }
void MiscChartsWidget::setCurrentTime(float t)  { currentTime_ = t; requestRefresh(); }
void MiscChartsWidget::setWindowSeconds(float s) { windowS_ = s; prevEndTime_ = -9999.0f; requestRefresh(); }
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
    const tnr::GraphSection sections[SECTIONS] = { tnr::GraphSection::MiscGForce,
        tnr::GraphSection::MiscRideHeight };
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
    const bool allTime = chartWindowIsTime(domains[GFORCE].window) &&
                         chartWindowIsTime(domains[RIDEHEIGHT].window);
    if (allTime) {
        const bool rebuild = dataModeKey_ != runtimeKey || endTime < prevEndTime_ ||
                             std::abs(endTime - prevEndTime_) > 1.0f;
        if (rebuild) {
            for (int id : { latId_, longId_, frontId_, rearId_, latRefId_, longRefId_, frontRefId_, rearRefId_ })
                chart_->clear(id);
            lastMotionT_ = domains[GFORCE].lower;
            lastMotionExT_ = domains[RIDEHEIGHT].lower;
            dataModeKey_ = runtimeKey;
        }
        auto motionStart = std::lower_bound(d.motionBuf.begin(), d.motionBuf.end(), lastMotionT_ + 0.0001f,
            [](const MotionSample& sample, float value) { return sample.t < value; });
        for (auto it = motionStart; it != d.motionBuf.end(); ++it) {
            if (it->t > endTime) break;
            chart_->appendPoint(latId_, it->t, it->g_lat);
            chart_->appendPoint(longId_, it->t, it->g_long);
            lastMotionT_ = it->t;
        }
        auto motionExStart = std::lower_bound(d.motionExBuf.begin(), d.motionExBuf.end(), lastMotionExT_ + 0.0001f,
            [](const MotionExSample& sample, float value) { return sample.t < value; });
        for (auto it = motionExStart; it != d.motionExBuf.end(); ++it) {
            if (it->t > endTime) break;
            chart_->appendPoint(frontId_, it->t, it->front_aero);
            chart_->appendPoint(rearId_, it->t, it->rear_aero);
            lastMotionExT_ = it->t;
        }
        chart_->trimBefore(latId_, domains[GFORCE].lower); chart_->trimBefore(longId_, domains[GFORCE].lower);
        chart_->trimBefore(frontId_, domains[RIDEHEIGHT].lower); chart_->trimBefore(rearId_, domains[RIDEHEIGHT].lower);
        for (int id : { latRefId_, longRefId_, frontRefId_, rearRefId_ }) chart_->setSeriesVisible(id, false);
    }
    const bool uniformNonTime = !allTime && domains[GFORCE].window == domains[RIDEHEIGHT].window &&
        domains[GFORCE].primary == domains[RIDEHEIGHT].primary &&
        domains[GFORCE].reference == domains[RIDEHEIGHT].reference &&
        domains[GFORCE].lower == domains[RIDEHEIGHT].lower;
    const bool nonTimeRebuild = !allTime && (dataModeKey_ != runtimeKey || endTime < prevEndTime_ ||
                                             std::abs(endTime - prevEndTime_) > 1.0f || !uniformNonTime);
    const QVector<MotionSample> motionSamples = nonTimeRebuild
        ? chartMotionSamples(d, domains[GFORCE]) : QVector<MotionSample>{};
    const QVector<MotionExSample> motionExSamples = nonTimeRebuild
        ? chartMotionExSamples(d, domains[RIDEHEIGHT]) : QVector<MotionExSample>{};
    auto fillMotion = [&](int primaryId, int referenceId, auto value) {
        if (!nonTimeRebuild) return;
        const ChartDomain& domain = domains[GFORCE];
        QVector<double> x, y, rx, ry;
        for (const MotionSample& sample : motionSamples) {
            const double key = domain.distance ? d.distanceAtTime(domain.primary, sample.t) : sample.t;
            if (!qIsFinite(key)) continue;
            x.push_back(key); y.push_back(value(sample));
        }
        if (domain.comparison && domain.reference) for (const MotionSample& sample : domain.reference->motion) {
            const double key = projectReferenceTime(d, domain, sample.t);
            if (qIsFinite(key)) { rx.push_back(key); ry.push_back(value(sample)); }
        }
        chart_->setSeriesData(primaryId, x, y); chart_->setSeriesData(referenceId, rx, ry);
        chart_->setSeriesVisible(referenceId, domain.comparison && chart_->seriesVisible(primaryId));
    };
    auto fillMotionEx = [&](int primaryId, int referenceId, auto value) {
        if (!nonTimeRebuild) return;
        const ChartDomain& domain = domains[RIDEHEIGHT];
        QVector<double> x, y, rx, ry;
        for (const MotionExSample& sample : motionExSamples) {
            const double key = domain.distance ? d.distanceAtTime(domain.primary, sample.t) : sample.t;
            if (!qIsFinite(key)) continue;
            x.push_back(key); y.push_back(value(sample));
        }
        if (domain.comparison && domain.reference) for (const MotionExSample& sample : domain.reference->motionEx) {
            const double key = projectReferenceTime(d, domain, sample.t);
            if (qIsFinite(key)) { rx.push_back(key); ry.push_back(value(sample)); }
        }
        chart_->setSeriesData(primaryId, x, y); chart_->setSeriesData(referenceId, rx, ry);
        chart_->setSeriesVisible(referenceId, domain.comparison && chart_->seriesVisible(primaryId));
    };
    fillMotion(latId_, latRefId_, [](const MotionSample& s) { return double(s.g_lat); });
    fillMotion(longId_, longRefId_, [](const MotionSample& s) { return double(s.g_long); });
    fillMotionEx(frontId_, frontRefId_, [](const MotionExSample& s) { return double(s.front_aero); });
    fillMotionEx(rearId_, rearRefId_, [](const MotionExSample& s) { return double(s.rear_aero); });
    if (nonTimeRebuild) {
        lastMotionT_ = motionSamples.isEmpty() ? float(domains[GFORCE].lower)
                                               : motionSamples.last().t;
        lastMotionExT_ = motionExSamples.isEmpty() ? float(domains[RIDEHEIGHT].lower)
                                                   : motionExSamples.last().t;
        dataModeKey_ = runtimeKey;
    } else if (!allTime && uniformNonTime) {
        const ChartDomain& motionDomain = domains[GFORCE];
        const QVector<MotionSample>& motion = motionDomain.distance && motionDomain.primary
            ? motionDomain.primary->motion : d.motionBuf;
        auto motionStart = std::lower_bound(motion.begin(), motion.end(), lastMotionT_ + 0.0001f,
            [](const MotionSample& sample, float value) { return sample.t < value; });
        for (auto it = motionStart; it != motion.end(); ++it) {
            if (it->t > endTime) break;
            if (it->t < motionDomain.lower) continue;
            const double key = motionDomain.distance ? d.distanceAtTime(motionDomain.primary, it->t) : it->t;
            if (!qIsFinite(key)) continue;
            chart_->appendPoint(latId_, key, it->g_lat); chart_->appendPoint(longId_, key, it->g_long);
            lastMotionT_ = it->t;
        }
        const ChartDomain& motionExDomain = domains[RIDEHEIGHT];
        const QVector<MotionExSample>& motionEx = motionExDomain.distance && motionExDomain.primary
            ? motionExDomain.primary->motionEx : d.motionExBuf;
        auto motionExStart = std::lower_bound(motionEx.begin(), motionEx.end(), lastMotionExT_ + 0.0001f,
            [](const MotionExSample& sample, float value) { return sample.t < value; });
        for (auto it = motionExStart; it != motionEx.end(); ++it) {
            if (it->t > endTime) break;
            if (it->t < motionExDomain.lower) continue;
            const double key = motionExDomain.distance ? d.distanceAtTime(motionExDomain.primary, it->t) : it->t;
            if (!qIsFinite(key)) continue;
            chart_->appendPoint(frontId_, key, it->front_aero); chart_->appendPoint(rearId_, key, it->rear_aero);
            lastMotionExT_ = it->t;
        }
    }

    // Feed any table-mode sections from the same window (newest sample on top).
    if (tableMode_[GFORCE] && visible_[GFORCE] && table_[GFORCE]) {
        GraphTable* t = table_[GFORCE];
        t->setDistanceMode(domains[GFORCE].distance);
        t->beginRebuild(domains[GFORCE].lower, domains[GFORCE].upper,
                        chartWindowAccumulatesLaps(domains[GFORCE].window));
        const QVector<MotionSample>& buf = domains[GFORCE].distance && domains[GFORCE].primary
            ? domains[GFORCE].primary->motion : d.motionBuf;
        for (int i = buf.size() - 1; i >= 0 && !t->full(); --i) {
            const auto& s = buf[i];
            if (s.t > domains[GFORCE].currentTime) continue;
            const double coordinate = domains[GFORCE].distance
                ? d.distanceAtTime(domains[GFORCE].primary, s.t) : s.t;
            if (!qIsFinite(coordinate) || coordinate < domains[GFORCE].lower ||
                coordinate > domains[GFORCE].upper) continue;
            t->addRow(coordinate, s.g_lat, s.g_long);
        }
        t->endRebuild();
    }
    if (tableMode_[RIDEHEIGHT] && visible_[RIDEHEIGHT] && table_[RIDEHEIGHT]) {
        GraphTable* t = table_[RIDEHEIGHT];
        t->setDistanceMode(domains[RIDEHEIGHT].distance);
        t->beginRebuild(domains[RIDEHEIGHT].lower, domains[RIDEHEIGHT].upper,
                        chartWindowAccumulatesLaps(domains[RIDEHEIGHT].window));
        const QVector<MotionExSample>& buf = domains[RIDEHEIGHT].distance && domains[RIDEHEIGHT].primary
            ? domains[RIDEHEIGHT].primary->motionEx : d.motionExBuf;
        for (int i = buf.size() - 1; i >= 0 && !t->full(); --i) {
            const auto& s = buf[i];
            if (s.t > domains[RIDEHEIGHT].currentTime) continue;
            const double coordinate = domains[RIDEHEIGHT].distance
                ? d.distanceAtTime(domains[RIDEHEIGHT].primary, s.t) : s.t;
            if (!qIsFinite(coordinate) || coordinate < domains[RIDEHEIGHT].lower ||
                coordinate > domains[RIDEHEIGHT].upper) continue;
            t->addRow(coordinate, s.front_aero, s.rear_aero);
        }
        t->endRebuild();
    }

    // ONE replot renders both panels — but skip it when the chart is hidden (every
    // section in table mode), so nothing renders off-screen. rebuildLayout() re-shows
    // it and requestRefresh()es on the way back to chart mode, so it repaints then.
    if (chart_->isVisible()) chart_->requestReplot();
    prevEndTime_ = endTime;
}
