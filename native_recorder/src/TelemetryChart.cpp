#include "TelemetryChart.h"
#include "SessionModel.h"

#include <QColor>
#include <QTimer>
#include <algorithm>

namespace {
const QColor C_SPEED("#37872D"), C_RPM("#C4162A"), C_ERS("#FADE2A");
// Reference (other-lap) traces are drawn dimmer so the current lap reads on top.
QColor muted(QColor c) { c.setAlpha(110); return c; }

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

QVector<TelemetryChart::LegendEntry> TelemetryChart::legendEntries()
{
    return { { "Speed", C_SPEED }, { "RPM", C_RPM }, { "ERS", C_ERS } };
}

TelemetryChart::TelemetryChart(QWidget* parent)
    : ChartView(parent)
{
    axXId_      = addAxis({ Side::Bottom, 0.0, windowS_, QColor(), true,  'f', 0, true });
    int axSpeed = addAxis({ Side::Left,   0.0, MAX_SPEED, C_SPEED, true,  'f', 0 });
    int axRpm   = addAxis({ Side::Right,  0.0, MAX_RPM,   C_RPM,   true,  'f', 0 });
    int axErs   = addAxis({ Side::Right,  0.0, 100.0,     C_ERS,   true,  'f', 0 });

    setAxisTimeTicker(axXId_, "%m:%s");             // m:ss.t labels, like Electron's fmtTime
    setAxisNumberSuffix(axRpm, 1000.0, "k", 2000);  // 16000 -> "16k", ticks every 2k
    setAxisNumberSuffix(axErs, 1.0, "%");           // 80    -> "80%"

    // Reference series first (drawn behind), then the current-lap series on top.
    rErId_ = addSeries({ "",      muted(C_ERS),   2.0, axXId_, axErs   });
    rRpId_ = addSeries({ "",      muted(C_RPM),   2.0, axXId_, axRpm   });
    rSpId_ = addSeries({ "",      muted(C_SPEED), 2.0, axXId_, axSpeed });
    erId_  = addSeries({ "ERS",   C_ERS,   2.5, axXId_, axErs,   "%",   1, false });
    rpId_  = addSeries({ "RPM",   C_RPM,   2.5, axXId_, axRpm,   "",    0, true  });
    spId_  = addSeries({ "Speed", C_SPEED, 2.5, axXId_, axSpeed, "kph", 0, false });

    showReference(false);
    setHoverReadout(true);
    setLegendVisible(false);   // legend lives in the toolbar (see OverviewPage)

    // Coalesces rebuilds to one per event-loop pass (i.e. one per arriving packet,
    // 20..60 Hz) via a zero-delay single-shot armed from requestRefresh() — no fixed
    // rate cap.
    refreshTimer_ = new QTimer(this);
    refreshTimer_->setSingleShot(true);
    refreshTimer_->setInterval(0);
    connect(refreshTimer_, &QTimer::timeout, this, [this] {
        // Only rebuild while shown; the model keeps accumulating either way, and
        // showEvent re-pulls when the Overview tab comes back into view.
        if (dirty_ && isVisible()) { dirty_ = false; refresh(); }
    });
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
    prevEndTime_ = -9999.0f;
    requestRefresh();
}

void TelemetryChart::requestRefresh() {
    dirty_ = true;
    if (!refreshTimer_->isActive()) refreshTimer_->start();
}

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
        case ChartMode::CurrentLap:  buildOverlay(currentLapBlock(), currentLapBlock(), now); break;
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
    if (std::abs(endTime - prevEndTime_) > 1.0f || endTime < prevEndTime_) {
        clear(spId_); clear(rpId_); clear(erId_);
        lastAddedTime_ = left;
        lastAddedStsTime_ = left;
    }
    auto lb = [](const auto& v, float t) {
        return std::lower_bound(v.begin(), v.end(), t, [](const auto& s, float key) { return s.t < key; });
    };
    int startIndex = std::distance(d.telBuf.begin(), lb(d.telBuf, lastAddedTime_ + 0.0001f));
    for (int i = startIndex; i < d.telBuf.size(); ++i) {
        const auto& s = d.telBuf[i];
        if (s.t > endTime) break;
        appendPoint(spId_, s.t, s.speed);
        appendPoint(rpId_, s.t, s.rpm);
        lastAddedTime_ = s.t;
    }
    int stsIndex = std::distance(d.stsBuf.begin(), lb(d.stsBuf, lastAddedStsTime_ + 0.0001f));
    for (int i = stsIndex; i < d.stsBuf.size(); ++i) {
        const auto& s = d.stsBuf[i];
        if (s.t > endTime) break;
        appendPoint(erId_, s.t, s.ers);
        lastAddedStsTime_ = s.t;
    }
    trimBefore(spId_, left);
    trimBefore(rpId_, left);
    trimBefore(erId_, left);
    prevEndTime_ = endTime;

    const double lo = std::max(0.0f, left);
    const double hi = std::max(windowS_, endTime);
    setXRange(axXId_, lo, hi > lo ? hi : lo + 1.0);
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
