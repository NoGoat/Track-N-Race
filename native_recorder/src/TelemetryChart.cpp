#include "TelemetryChart.h"
#include "SessionModel.h"

#include <QColor>
#include <QTimer>
#include <algorithm>

namespace {
const QColor C_SPEED("#37872D"), C_RPM("#C4162A"), C_ERS("#FADE2A");
// Reference (other-lap) traces are drawn dimmer so the current lap reads on top.
QColor muted(QColor c) { c.setAlpha(110); return c; }

// Append a lap's samples (those with t <= upTo) as (t - originT, value) pairs.
void fillLap(const LapBlock* lap, float originT, float upTo,
             QVector<double>& tx, QVector<double>& spd, QVector<double>& rpm,
             QVector<double>& ex, QVector<double>& ers) {
    if (!lap) return;
    for (const TelSample& s : lap->tel) {
        if (s.t > upTo) break;
        tx.append(s.t - originT); spd.append(s.speed); rpm.append(s.rpm);
    }
    for (const StsSample& s : lap->sts) {
        if (s.t > upTo) break;
        ex.append(s.t - originT); ers.append(s.ers);
    }
}
}

TelemetryChart::TelemetryChart(QWidget* parent)
    : ChartView(parent)
{
    axXId_      = addAxis({ Side::Bottom, 0.0, windowS_, QColor(), true,  'f', 0 });
    int axSpeed = addAxis({ Side::Left,   0.0, MAX_SPEED, C_SPEED, true,  'f', 0 });
    int axRpm   = addAxis({ Side::Right,  0.0, MAX_RPM,   C_RPM,   true,  'f', 0 });
    int axErs   = addAxis({ Side::Right,  0.0, 100.0,     C_ERS,   false, 'f', 0 });

    setAxisTimeTicker(axXId_, "%m:%s");   // mm:ss labels, like Electron's fmtTime

    // Reference series first (drawn behind), then the current-lap series on top.
    rErId_ = addSeries({ "",      muted(C_ERS),   2.0, axXId_, axErs   });
    rRpId_ = addSeries({ "",      muted(C_RPM),   2.0, axXId_, axRpm   });
    rSpId_ = addSeries({ "",      muted(C_SPEED), 2.0, axXId_, axSpeed });
    erId_  = addSeries({ "ERS",   C_ERS,   2.5, axXId_, axErs   });
    rpId_  = addSeries({ "RPM",   C_RPM,   2.5, axXId_, axRpm   });
    spId_  = addSeries({ "Speed", C_SPEED, 2.5, axXId_, axSpeed });

    showReference(false);
    setHoverReadout(true);

    refreshTimer_ = new QTimer(this);
    refreshTimer_->setInterval(33);   // ~30 Hz, coalesces rebuilds
    connect(refreshTimer_, &QTimer::timeout, this, [this] {
        // Only rebuild while shown; the model keeps accumulating either way, and
        // showEvent re-pulls when the Overview tab comes back into view.
        if (dirty_ && isVisible()) { dirty_ = false; refresh(); }
    });
    refreshTimer_->start();
}

void TelemetryChart::showEvent(QShowEvent* e)
{
    ChartView::showEvent(e);
    requestRefresh();
}

void TelemetryChart::setModel(SessionModel* m)
{
    model_ = m;
    if (!m) return;
    connect(m, &SessionModel::telemetryAppended, this, &TelemetryChart::requestRefresh);
    connect(m, &SessionModel::lapsChanged,       this, &TelemetryChart::requestRefresh);
    connect(m, &SessionModel::wasReset,          this, &TelemetryChart::requestRefresh);
    requestRefresh();
}

void TelemetryChart::setPlaybackMode(bool on) { playback_ = on; requestRefresh(); }
void TelemetryChart::setCurrentTime(float t)  { currentTime_ = t; requestRefresh(); }
void TelemetryChart::setMode(ChartMode m)     { mode_ = m; requestRefresh(); }
void TelemetryChart::setCompareLap(int n)     { compareLap_ = n; requestRefresh(); }

void TelemetryChart::setWindowSeconds(float seconds)
{
    windowS_ = seconds;
    requestRefresh();
}

void TelemetryChart::requestRefresh() { dirty_ = true; }

float TelemetryChart::currentTime() const
{
    if (playback_) return currentTime_;
    return model_ ? model_->data().latestTime : 0.0f;
}

const LapBlock* TelemetryChart::currentLapBlock() const
{
    if (!model_) return nullptr;
    const SessionData& d = model_->data();
    if (playback_) return d.lapAtTime(currentTime());
    return d.curLapNum >= 0 ? &d.curLap : nullptr;
}

const LapBlock* TelemetryChart::previousLapBlock() const
{
    const LapBlock* cur = currentLapBlock();
    if (!model_ || !cur) return nullptr;
    return model_->data().lapByNum(cur->lapNum - 1);
}

void TelemetryChart::showReference(bool on)
{
    setSeriesVisible(rSpId_, on);
    setSeriesVisible(rRpId_, on);
    setSeriesVisible(rErId_, on);
}

void TelemetryChart::refresh()
{
    if (!model_) return;
    const float now = currentTime();
    switch (mode_) {
        case ChartMode::Default:     buildDefault(now); break;
        case ChartMode::CurrentLap:  buildSingleLap(currentLapBlock(), now); break;
        case ChartMode::PreviousLap: buildOverlay(previousLapBlock(), currentLapBlock(), now); break;
        case ChartMode::FastestLap:  buildOverlay(model_->data().fastestLap(), currentLapBlock(), now); break;
        case ChartMode::Compare:     buildOverlay(model_->data().lapByNum(compareLap_), currentLapBlock(), now); break;
    }
    requestReplot();
}

void TelemetryChart::buildDefault(float endTime)
{
    showReference(false);
    const SessionData& d = model_->data();
    const float left = endTime - windowS_;

    // Buffers are time-sorted, so binary-search the window instead of scanning
    // the whole (full-session, in playback) buffer each refresh.
    QVector<double> tx, spd, rpm, ex, ers;
    auto lb = [](const auto& v, float t) {
        return std::lower_bound(v.begin(), v.end(), t,
            [](const auto& s, float key) { return s.t < key; });
    };
    for (auto it = lb(d.telBuf, left); it != d.telBuf.end() && it->t <= endTime; ++it) {
        tx.append(it->t); spd.append(it->speed); rpm.append(it->rpm);
    }
    for (auto it = lb(d.stsBuf, left); it != d.stsBuf.end() && it->t <= endTime; ++it) {
        ex.append(it->t); ers.append(it->ers);
    }
    setSeriesData(spId_, tx, spd);
    setSeriesData(rpId_, tx, rpm);
    setSeriesData(erId_, ex, ers);

    const double lo = tx.isEmpty() ? 0.0 : std::max<double>(left, tx.first());
    const double hi = tx.isEmpty() ? windowS_ : endTime;
    setXRange(axXId_, lo, hi > lo ? hi : lo + 1.0);
}

void TelemetryChart::buildSingleLap(const LapBlock* lap, float upTo)
{
    showReference(false);
    QVector<double> tx, spd, rpm, ex, ers;
    fillLap(lap, lap ? lap->startSessionTime : 0.0f, upTo, tx, spd, rpm, ex, ers);
    setSeriesData(spId_, tx, spd);
    setSeriesData(rpId_, tx, rpm);
    setSeriesData(erId_, ex, ers);

    const double dur = lap ? (lap->endSessionTime - lap->startSessionTime) : windowS_;
    setXRange(axXId_, 0.0, dur > 1.0 ? dur : windowS_);
}

void TelemetryChart::buildOverlay(const LapBlock* ref, const LapBlock* cur, float curUpTo)
{
    // Current lap (full colour).
    QVector<double> tx, spd, rpm, ex, ers;
    fillLap(cur, cur ? cur->startSessionTime : 0.0f, curUpTo, tx, spd, rpm, ex, ers);
    setSeriesData(spId_, tx, spd);
    setSeriesData(rpId_, tx, rpm);
    setSeriesData(erId_, ex, ers);

    // Reference lap (muted) — shown in full, aligned by time-from-lap-start.
    const bool haveRef = ref && !ref->tel.isEmpty();
    showReference(haveRef);
    double refDur = 0.0;
    if (haveRef) {
        QVector<double> rtx, rspd, rrpm, rex, rers;
        fillLap(ref, ref->startSessionTime, ref->endSessionTime, rtx, rspd, rrpm, rex, rers);
        setSeriesData(rSpId_, rtx, rspd);
        setSeriesData(rRpId_, rtx, rrpm);
        setSeriesData(rErId_, rex, rers);
        refDur = ref->endSessionTime - ref->startSessionTime;
    }

    const double curDur = (cur && !tx.isEmpty()) ? tx.last() : 0.0;
    const double dur = std::max({ refDur, curDur, 1.0 });
    setXRange(axXId_, 0.0, dur);
}
