#include "GForceChart.h"
#include "../SessionModel.h"

#include <QTimer>

GForceChart::GForceChart(QWidget* parent) : ChartView(parent) {
    refreshTimer_ = new QTimer(this);
    refreshTimer_->setSingleShot(true);
    refreshTimer_->setInterval(33);
    connect(refreshTimer_, &QTimer::timeout, this, &GForceChart::refresh);

    ChartView::AxisSpec xAx;
    xAx.side = ChartView::Side::Bottom;
    xAx.grid = true;
    xAx.numberFormat = 'f';
    xAx.precision = 1;
    axXId_ = addAxis(xAx);
    setAxisTimeTicker(axXId_, "%m:%s");

    ChartView::AxisSpec yAx;
    yAx.side = ChartView::Side::Left;
    yAx.min = -6.0;
    yAx.max = 6.0;
    yAx.grid = true;
    int axYId = addAxis(yAx);
    setAxisNumberSuffix(axYId, 1.0, " G", 1.0);

    ChartView::SeriesSpec latSpec;
    latSpec.name = "Lateral G";
    latSpec.color = QColor("#F0A500");
    latSpec.xAxisId = axXId_;
    latSpec.yAxisId = axYId;
    latSpec.unit = " G";
    latSpec.tipPrecision = 2;
    latId_ = addSeries(latSpec);

    ChartView::SeriesSpec longSpec;
    longSpec.name = "Longitudinal G";
    longSpec.color = QColor("#5794F2");
    longSpec.xAxisId = axXId_;
    longSpec.yAxisId = axYId;
    longSpec.unit = " G";
    longSpec.tipPrecision = 2;
    longId_ = addSeries(longSpec);

    setHoverReadout(true);
}

void GForceChart::setModel(SessionModel* m) {
    if (model_) {
        disconnect(model_, nullptr, this, nullptr);
    }
    model_ = m;
    if (model_) {
        connect(model_, &SessionModel::telemetryAppended, this, &GForceChart::requestRefresh);
        connect(model_, &SessionModel::wasReset, this, [this] {
            lastAddedTime_ = -1.0f;
            prevEndTime_ = -1.0f;
            clearAll();
            requestRefresh();
        });
    }
    requestRefresh();
}

void GForceChart::setPlaybackMode(bool on) {
    if (playback_ == on) return;
    playback_ = on;
    lastAddedTime_ = -1.0f;
    prevEndTime_ = -1.0f;
    requestRefresh();
}

void GForceChart::setWindowSeconds(float seconds) {
    if (qFuzzyCompare(windowS_, seconds)) return;
    windowS_ = seconds;
    prevEndTime_ = -1.0f;
    requestRefresh();
}

void GForceChart::setCurrentTime(float t) {
    if (qFuzzyCompare(currentTime_, t)) return;
    currentTime_ = t;
    if (playback_) requestRefresh();
}

void GForceChart::showEvent(QShowEvent* e) {
    ChartView::showEvent(e);
    requestRefresh();
}

void GForceChart::requestRefresh() {
    if (!isVisible()) return;
    dirty_ = true;
    if (!refreshTimer_->isActive()) {
        refreshTimer_->start();
    }
}

float GForceChart::currentTime() const {
    if (playback_) return currentTime_;
    return model_ ? model_->data().latestTime : 0.0f;
}

void GForceChart::refresh() {
    if (!dirty_ || !model_) return;
    dirty_ = false;

    float cTime = currentTime();
    if (cTime < prevEndTime_) {
        lastAddedTime_ = -1.0f;
        clearAll();
    }
    
    buildDefault(cTime);

    float startTime = qMax(0.0f, cTime - windowS_);
    setXRange(axXId_, startTime, startTime + windowS_);
    requestReplot();
}

void GForceChart::buildDefault(float endTime) {
    const auto& buf = model_->data().motionBuf;
    if (buf.isEmpty()) return;

    int startIndex = 0;
    if (lastAddedTime_ >= 0) {
        auto it = std::lower_bound(buf.begin(), buf.end(), lastAddedTime_,
            [](const MotionSample& s, float t) { return s.t < t; });
        startIndex = std::distance(buf.begin(), it);
    }

    if (startIndex < buf.size()) {
        QVector<double> ts, lats, longs;
        ts.reserve(buf.size() - startIndex);
        lats.reserve(buf.size() - startIndex);
        longs.reserve(buf.size() - startIndex);

        for (int i = startIndex; i < buf.size(); ++i) {
            const auto& s = buf[i];
            if (s.t > endTime) break;
            ts.push_back(s.t);
            lats.push_back(s.g_lat);
            longs.push_back(s.g_long);
            lastAddedTime_ = s.t;
        }

        if (!ts.isEmpty()) {
            putSeries(latId_, ts, lats);
            putSeries(longId_, ts, longs);
        }
    }

    float startTime = qMax(0.0f, endTime - windowS_);
    trimBefore(latId_, startTime);
    trimBefore(longId_, startTime);
    prevEndTime_ = endTime;
}

void GForceChart::putSeries(int id, const QVector<double>& xs, const QVector<double>& ys) {
    if (lastAddedTime_ < 0) {
        setSeriesData(id, xs, ys);
    } else {
        for (int i = 0; i < xs.size(); ++i) {
            appendPoint(id, xs[i], ys[i]);
        }
    }
}
