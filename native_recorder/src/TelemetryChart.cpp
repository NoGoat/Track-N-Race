#include "TelemetryChart.h"
#include "SessionModel.h"

#include <QColor>
#include <QTimer>
#include <algorithm>

namespace {
const QColor C_SPEED("#37872D"), C_RPM("#C4162A");
const double RPM_K = 1.0 / 1000.0;   // RPM axis runs 0–16 ("16k") → scale data by 1/1000
// Reference (other-lap) traces are drawn dimmer so the current lap reads on top.
QColor muted(QColor c) { c.setAlpha(110); return c; }

void fillLap(const LapBlock* lap, float originT, float upTo,
             QVector<double>& tx, QVector<double>& spd, QVector<double>& rpm) {
    if (!lap) return;
    for (const TelSample& s : lap->tel) {
        if (s.t > upTo) break;
        tx.append(s.t - originT); spd.append(s.speed); rpm.append(s.rpm);
    }
}
}

QVector<TelemetryChart::LegendEntry> TelemetryChart::legendEntries()
{
    return { { "Speed", C_SPEED }, { "RPM", C_RPM } };
}

TelemetryChart::TelemetryChart(QWidget* parent)
    : ChartView(parent)
{
    // X is a native QDateTimeAxis ("m:ss"); Speed/RPM are native value axes with no
    // colour, so Qt draws the whole cartesian grid + labels itself. RPM runs 0–16
    // (data scaled by RPM_K) so its native "%.0fk" format reads "16k". Only the two
    // primary axes (X + Speed) carry grid lines; RPM is labels-only to avoid a second,
    // differently-scaled horizontal grid.
    // Only X carries grid lines (vertical); both Y axes are labels-only (grid=false)
    // so there are no horizontal grid lines to misalign with one axis or the other.
    axXId_      = addAxis({ Side::Bottom, 0.0, windowS_,        QColor(), true, 'f', 0, true,  true });
    int axSpeed = addAxis({ Side::Left,   0.0, MAX_SPEED,       QColor(), true, 'f', 0, false });
    int axRpm   = addAxis({ Side::Right,  0.0, MAX_RPM * RPM_K, QColor(), true, 'f', 0, false });

    setAxisNumberSuffix(axRpm, 1.0, "k", 2);   // axis 0–16 → "0k".."16k", ticks every 2k

    ChartView::SeriesSpec rRpm{ "", muted(C_RPM), 2.0, axXId_, axRpm };
    rRpm.yScale = RPM_K;
    ChartView::SeriesSpec rpm{ "RPM", C_RPM, 2.5, axXId_, axRpm, "", 0, true };
    rpm.yScale = RPM_K;

    // Reference series first (drawn behind), then the current-lap series on top.
    rRpId_ = addSeries(rRpm);
    rSpId_ = addSeries({ "",      muted(C_SPEED), 2.0, axXId_, axSpeed });
    rpId_  = addSeries(rpm);
    spId_  = addSeries({ "Speed", C_SPEED, 2.5, axXId_, axSpeed, "kph", 0, false });

    showReference(false);
    setHoverReadout(true);
    setLegendVisible(false);   // legend lives in the toolbar (see OverviewPage)

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
    prevEndTime_ = -9999.0f;
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
        clear(spId_); clear(rpId_);
        lastAddedTime_ = left;
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
    trimBefore(spId_, left);
    trimBefore(rpId_, left);
    prevEndTime_ = endTime;

    const double lo = std::max(0.0f, left);
    const double hi = std::max(windowS_, endTime);
    setXRange(axXId_, lo, hi > lo ? hi : lo + 1.0);
}

void TelemetryChart::buildOverlay(const LapBlock* ref, const LapBlock* cur, float curUpTo)
{
    // Current lap (full colour).
    QVector<double> tx, spd, rpm;
    fillLap(cur, cur ? cur->startSessionTime : 0.0f, curUpTo, tx, spd, rpm);
    setSeriesData(spId_, tx, spd);
    setSeriesData(rpId_, tx, rpm);

    // Reference lap (muted) — shown in full, aligned by time-from-lap-start.
    const bool haveRef = ref && !ref->tel.isEmpty();
    showReference(haveRef);
    double refDur = 0.0;
    if (haveRef) {
        QVector<double> rtx, rspd, rrpm;
        fillLap(ref, ref->startSessionTime, ref->endSessionTime, rtx, rspd, rrpm);
        setSeriesData(rSpId_, rtx, rspd);
        setSeriesData(rRpId_, rtx, rrpm);
        refDur = ref->endSessionTime - ref->startSessionTime;
    }

    const double curDur = (cur && !tx.isEmpty()) ? tx.last() : 0.0;
    const double dur = std::max({ refDur, curDur, 1.0 });
    setXRange(axXId_, 0.0, dur);
}
