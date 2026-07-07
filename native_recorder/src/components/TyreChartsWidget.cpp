#include "TyreChartsWidget.h"
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
    // QCustomPlot / OpenGL context / replot rather than four separate widgets. Each
    // panel carries its own in-plot title and FL/FR/RL/RR colour-key legend.
    // Table-mode sections render as GraphTables overlaid in the same grid cell (see
    // rebuildLayout).
    chart_ = new ChartView;

    auto addSection = [&](int sec, const QString& title, double yMin, double yMax,
                          const QString& unit) {
        if (sec > 0) chart_->addPanel();   // panel 0 already exists in the ChartView
        xId_[sec] = chart_->addAxis(
            { ChartView::Side::Bottom, 0.0, windowS_, QColor(), true, 'f', 0, true }, sec);
        const int yId = chart_->addAxis(
            { ChartView::Side::Left, yMin, yMax, QColor(), true, 'f', 0 }, sec);
        chart_->setAxisTimeTicker(xId_[sec], "%m:%s");
        // Build the header (title + colour key) before the series, so the series
        // register in this panel's key legend rather than the default one.
        chart_->setPanelTitle(sec, title);
        chart_->setPanelLegendVisible(sec, true);
        for (int w = 0; w < 4; ++w) {
            seriesIds_[sec][w] = chart_->addSeries({
                kWheelNames[w], kWheelColors[w], 1.5, xId_[sec], yId, unit, 0
            });
        }
    };

    addSection(SURF,  "SURFACE TEMP", 0, 200,  "°C");
    addSection(INNER, "INNER TEMP",   0, 200,  "°C");
    addSection(BRAKE, "BRAKE TEMP",   0, 1200, "°C");
    addSection(WEAR,  lifeMode_ ? "TYRE LIFE" : "TYRE WEAR", 0, 100, "%");

    // 2×2 grid for the fullscreen Tyres view, 1×4 row for the Overview strip.
    rebuildLayout();

    // Enable hover once every panel/axis exists (crosshairs are created per panel).
    chart_->setHoverReadout(true);

    // Zero-delay single-shot armed from requestRefresh(): coalesces to one rebuild
    // per event-loop pass (one per arriving packet, 20..60 Hz), no fixed rate cap.
    refreshTimer_ = new QTimer(this);
    refreshTimer_->setSingleShot(true);
    refreshTimer_->setInterval(0);
    connect(refreshTimer_, &QTimer::timeout, this, [this] {
        if (dirty_ && isVisible()) { dirty_ = false; refresh(); }
    });
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
    requestRefresh();
}

void TyreChartsWidget::setPlaybackMode(bool on) { playback_ = on; requestRefresh(); }
void TyreChartsWidget::setCurrentTime(float t)  { currentTime_ = t; requestRefresh(); }
void TyreChartsWidget::setWindowSeconds(float s) { windowS_ = s; prevEndTime_ = -9999.0f; requestRefresh(); }
void TyreChartsWidget::requestRefresh() { dirty_ = true; if (!refreshTimer_->isActive()) refreshTimer_->start(); }

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
    const float left    = endTime - windowS_;

    if (std::abs(endTime - prevEndTime_) > 1.0f || endTime < prevEndTime_) {
        for (int s = 0; s < SECTIONS; ++s)
            for (int w = 0; w < 4; ++w)
                chart_->clear(seriesIds_[s][w]);
        lastAddedTime_ = left;
    }

    auto lb = [](const auto& v, float t) {
        return std::lower_bound(v.begin(), v.end(), t,
            [](const auto& s, float key) { return s.t < key; });
    };

    int startIdx = (int)std::distance(d.tyreBuf.begin(), lb(d.tyreBuf, lastAddedTime_ + 0.0001f));
    for (int i = startIdx; i < d.tyreBuf.size(); ++i) {
        const auto& s = d.tyreBuf[i];
        if (s.t > endTime) break;

        chart_->appendPoint(seriesIds_[SURF][0], s.t, s.surfFl);
        chart_->appendPoint(seriesIds_[SURF][1], s.t, s.surfFr);
        chart_->appendPoint(seriesIds_[SURF][2], s.t, s.surfRl);
        chart_->appendPoint(seriesIds_[SURF][3], s.t, s.surfRr);

        chart_->appendPoint(seriesIds_[INNER][0], s.t, s.innerFl);
        chart_->appendPoint(seriesIds_[INNER][1], s.t, s.innerFr);
        chart_->appendPoint(seriesIds_[INNER][2], s.t, s.innerRl);
        chart_->appendPoint(seriesIds_[INNER][3], s.t, s.innerRr);

        chart_->appendPoint(seriesIds_[BRAKE][0], s.t, s.brakeFl);
        chart_->appendPoint(seriesIds_[BRAKE][1], s.t, s.brakeFr);
        chart_->appendPoint(seriesIds_[BRAKE][2], s.t, s.brakeRl);
        chart_->appendPoint(seriesIds_[BRAKE][3], s.t, s.brakeRr);

        // Life mode plots remaining tyre life (100 - wear); wear mode plots the
        // accumulated wear directly. Matches the Electron tyreWearMode toggle.
        const auto wv = [this](float w) { return lifeMode_ ? 100.0f - w : w; };
        chart_->appendPoint(seriesIds_[WEAR][0], s.t, wv(s.wearFl));
        chart_->appendPoint(seriesIds_[WEAR][1], s.t, wv(s.wearFr));
        chart_->appendPoint(seriesIds_[WEAR][2], s.t, wv(s.wearRl));
        chart_->appendPoint(seriesIds_[WEAR][3], s.t, wv(s.wearRr));

        lastAddedTime_ = s.t;
    }

    for (int s = 0; s < SECTIONS; ++s)
        for (int w = 0; w < 4; ++w)
            chart_->trimBefore(seriesIds_[s][w], left);

    const double lo = (double)std::max(0.0f, left);
    const double hi = (double)std::max(windowS_, endTime);
    const double hiClamped = hi > lo ? hi : lo + 1.0;
    for (int s = 0; s < SECTIONS; ++s)
        chart_->setXRange(xId_[s], lo, hiClamped);

    // Feed any table-mode sections from the same window (newest sample on top).
    bool anyTable = false;
    for (int s = 0; s < SECTIONS; ++s)
        if (tableMode_[s] && visible_[s] && table_[s]) anyTable = true;
    if (anyTable) {
        const auto wv = [this](float w) { return lifeMode_ ? 100.0f - w : w; };
        for (int s = 0; s < SECTIONS; ++s)
            if (tableMode_[s] && visible_[s] && table_[s]) table_[s]->beginRebuild();
        for (int i = d.tyreBuf.size() - 1; i >= 0; --i) {
            const auto& s = d.tyreBuf[i];
            if (s.t > endTime) continue;
            if (s.t < left)    break;
            auto add4 = [&](int sec, float fl, float fr, float rl, float rr) {
                if (!(tableMode_[sec] && visible_[sec] && table_[sec]) || table_[sec]->full()) return;
                table_[sec]->addRow(s.t, fl, fr, rl, rr);
            };
            add4(SURF,  s.surfFl,  s.surfFr,  s.surfRl,  s.surfRr);
            add4(INNER, s.innerFl, s.innerFr, s.innerRl, s.innerRr);
            add4(BRAKE, s.brakeFl, s.brakeFr, s.brakeRl, s.brakeRr);
            add4(WEAR,  wv(s.wearFl), wv(s.wearFr), wv(s.wearRl), wv(s.wearRr));
            bool allFull = true;
            for (int sec = 0; sec < SECTIONS; ++sec)
                if (tableMode_[sec] && visible_[sec] && table_[sec] && !table_[sec]->full())
                    { allFull = false; break; }
            if (allFull) break;
        }
        for (int s = 0; s < SECTIONS; ++s)
            if (tableMode_[s] && visible_[s] && table_[s]) table_[s]->endRebuild();
    }

    chart_->requestReplot();   // ONE replot renders all four panels
    prevEndTime_ = endTime;
}
