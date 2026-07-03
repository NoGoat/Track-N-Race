#include "TyreChartsWidget.h"
#include "ChartView.h"
#include "../SessionModel.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFrame>
#include <QLabel>
#include <QFont>
#include <QPalette>
#include <QColor>
#include <QTimer>
#include <QShowEvent>
#include <algorithm>

namespace {
const QColor C_FL("#e10600");
const QColor C_FR("#4488ff");
const QColor C_RL("#37872D");
const QColor C_RR("#ffd700");
const QColor kWheelColors[4] = { C_FL, C_FR, C_RL, C_RR };
const char*  kWheelNames[4]  = { "FL", "FR", "RL", "RR" };
}

TyreChartsWidget::TyreChartsWidget(QWidget* parent)
    : QWidget(parent)
{
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    // 4 charts side by side
    auto* row = new QWidget;
    auto* hbox = new QHBoxLayout(row);
    hbox->setContentsMargins(0, 0, 0, 0);
    hbox->setSpacing(0);

    int sectionIdx = 0;
    auto addSection = [&](const QString& title, double yMin, double yMax,
                          const QString& unit, ChartView*& outChart, int* outIds, int& outXId,
                          QLabel** outTitle) {
        if (sectionIdx > 0) {
            auto* line = new QFrame;
            line->setFrameShape(QFrame::VLine);
            line->setFrameShadow(QFrame::Sunken);
            dividers_[sectionIdx - 1] = line;
            hbox->addWidget(line);
        }
        auto* section = new QWidget;
        auto* sl = new QVBoxLayout(section);
        sl->setContentsMargins(0, 0, 0, 0);
        sl->setSpacing(0);

        auto* header = new QWidget;
        header->setFixedHeight(24);
        auto* hl = new QHBoxLayout(header);
        hl->setContentsMargins(6, 0, 6, 0);
        auto* titleLabel = new QLabel(title);
        QFont f; f.setPointSize(8); f.setBold(true);
        titleLabel->setFont(f);
        titleLabel->setForegroundRole(QPalette::PlaceholderText);
        hl->addWidget(titleLabel);
        if (outTitle) *outTitle = titleLabel;
        hl->addStretch();
        for (int i = 0; i < 4; ++i) {
            auto* sw = new QWidget;
            sw->setFixedSize(10, 10);
            sw->setStyleSheet(QString("background-color: %1; border-radius: 2px;").arg(kWheelColors[i].name()));
            hl->addWidget(sw);
            auto* lbl = new QLabel(kWheelNames[i]);
            QFont lf; lf.setPointSize(8);
            lbl->setFont(lf);
            lbl->setForegroundRole(QPalette::PlaceholderText);
            hl->addWidget(lbl);
            hl->addSpacing(4);
        }
        sl->addWidget(header);

        outChart = new ChartView;
        outXId = outChart->addAxis({ ChartView::Side::Bottom, 0.0, windowS_, QColor(), true, 'f', 0, true });
        int yId = outChart->addAxis({ ChartView::Side::Left, yMin, yMax, QColor(), true, 'f', 0 });
        outChart->setAxisTimeTicker(outXId, "%m:%s");
        for (int w = 0; w < 4; ++w) {
            outIds[w] = outChart->addSeries({
                kWheelNames[w], kWheelColors[w], 1.5, outXId, yId, unit, 0
            });
        }
        outChart->setHoverReadout(true);
        outChart->setLegendVisible(false);
        sl->addWidget(outChart, 1);
        sections_[sectionIdx] = section;
        hbox->addWidget(section, 1);
        ++sectionIdx;
    };

    addSection("SURFACE TEMP", 0, 200,  "°C", surfChart_,  surfIds_,  surfXId_,  nullptr);
    addSection("INNER TEMP",   0, 200,  "°C", innerChart_, innerIds_, innerXId_, nullptr);
    addSection("BRAKE TEMP",   0, 1200, "°C", brakeChart_, brakeIds_, brakeXId_, nullptr);
    addSection(lifeMode_ ? "TYRE LIFE" : "TYRE WEAR",
               0, 100, "%", wearChart_, wearIds_, wearXId_, &wearTitle_);

    outer->addWidget(row, 1);

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
    if (wearTitle_) wearTitle_->setText(life ? "TYRE LIFE" : "TYRE WEAR");
    prevEndTime_ = -9999.0f;   // force a full clear + re-append with the new mapping
    requestRefresh();
}

void TyreChartsWidget::setChartSectionVisible(int i, bool on)
{
    if (i < 0 || i >= 4) return;
    if (sections_[i]) sections_[i]->setVisible(on);
    updateDividers();
}

void TyreChartsWidget::updateDividers()
{
    for (int d = 0; d < 3; ++d) {
        if (dividers_[d])
            dividers_[d]->setVisible(
                sections_[d]   && sections_[d]->isVisible() &&
                sections_[d+1] && sections_[d+1]->isVisible());
    }
}

float TyreChartsWidget::currentTime() const {
    if (playback_) return currentTime_;
    return model_ ? model_->data().latestTime : 0.0f;
}

void TyreChartsWidget::refresh() {
    if (!model_) return;

    const SessionData& d = model_->data();
    const float endTime = currentTime();
    const float left    = endTime - windowS_;

    if (std::abs(endTime - prevEndTime_) > 1.0f || endTime < prevEndTime_) {
        for (int w = 0; w < 4; ++w) {
            surfChart_->clear(surfIds_[w]);
            innerChart_->clear(innerIds_[w]);
            brakeChart_->clear(brakeIds_[w]);
            wearChart_->clear(wearIds_[w]);
        }
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

        surfChart_->appendPoint(surfIds_[0], s.t, s.surfFl);
        surfChart_->appendPoint(surfIds_[1], s.t, s.surfFr);
        surfChart_->appendPoint(surfIds_[2], s.t, s.surfRl);
        surfChart_->appendPoint(surfIds_[3], s.t, s.surfRr);

        innerChart_->appendPoint(innerIds_[0], s.t, s.innerFl);
        innerChart_->appendPoint(innerIds_[1], s.t, s.innerFr);
        innerChart_->appendPoint(innerIds_[2], s.t, s.innerRl);
        innerChart_->appendPoint(innerIds_[3], s.t, s.innerRr);

        brakeChart_->appendPoint(brakeIds_[0], s.t, s.brakeFl);
        brakeChart_->appendPoint(brakeIds_[1], s.t, s.brakeFr);
        brakeChart_->appendPoint(brakeIds_[2], s.t, s.brakeRl);
        brakeChart_->appendPoint(brakeIds_[3], s.t, s.brakeRr);

        // Life mode plots remaining tyre life (100 - wear); wear mode plots the
        // accumulated wear directly. Matches the Electron tyreWearMode toggle.
        const auto wv = [this](float w) { return lifeMode_ ? 100.0f - w : w; };
        wearChart_->appendPoint(wearIds_[0], s.t, wv(s.wearFl));
        wearChart_->appendPoint(wearIds_[1], s.t, wv(s.wearFr));
        wearChart_->appendPoint(wearIds_[2], s.t, wv(s.wearRl));
        wearChart_->appendPoint(wearIds_[3], s.t, wv(s.wearRr));

        lastAddedTime_ = s.t;
    }

    for (int w = 0; w < 4; ++w) {
        surfChart_->trimBefore(surfIds_[w],  left);
        innerChart_->trimBefore(innerIds_[w], left);
        brakeChart_->trimBefore(brakeIds_[w], left);
        wearChart_->trimBefore(wearIds_[w],  left);
    }

    const double lo = (double)std::max(0.0f, left);
    const double hi = (double)std::max(windowS_, endTime);
    const double hiClamped = hi > lo ? hi : lo + 1.0;
    surfChart_->setXRange(surfXId_,   lo, hiClamped);
    innerChart_->setXRange(innerXId_, lo, hiClamped);
    brakeChart_->setXRange(brakeXId_, lo, hiClamped);
    wearChart_->setXRange(wearXId_,  lo, hiClamped);

    surfChart_->requestReplot();
    innerChart_->requestReplot();
    brakeChart_->requestReplot();
    wearChart_->requestReplot();

    prevEndTime_ = endTime;
}
