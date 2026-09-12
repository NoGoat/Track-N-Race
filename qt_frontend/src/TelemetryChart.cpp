#include "TelemetryChart.h"
#include "SessionModel.h"
#include "ChartCoordinates.h"
#include "PresentationScheduler.h"

#include <QColor>
#include <QtMath>
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
    // ERS comes from the 2 Hz CAR_STATUS packet — draw it stepped (hold-last-value)
    // rather than interpolating across the half-second gaps between samples.
    rErId_ = addSeries({ "",      muted(C_ERS),   2.0, axXId_, axErs,   "",    0, false, false, QColor(), true });
    rRpId_ = addSeries({ "",      muted(C_RPM),   2.0, axXId_, axRpm   });
    rSpId_ = addSeries({ "",      muted(C_SPEED), 2.0, axXId_, axSpeed });
    erId_  = addSeries({ "ERS",   C_ERS,   2.5, axXId_, axErs,   "%",   1, false, false, QColor(), true });
    rpId_  = addSeries({ "RPM",   C_RPM,   2.5, axXId_, axRpm,   "",    0, true  });
    spId_  = addSeries({ "Speed", C_SPEED, 2.5, axXId_, axSpeed, "kph", 0, false });

    showReference(false);
    setHoverReadout(true);
    setPanelTitle(0, "SPEED / RPM / ERS");
    setPanelLegendVisible(0, true);
    linkSeriesVisibility(spId_, rSpId_);
    linkSeriesVisibility(rpId_, rRpId_);
    linkSeriesVisibility(erId_, rErId_);

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
    connect(m, &SessionModel::chartConfigurationChanged, this, &TelemetryChart::requestRefresh);
    bindPanelChartSettings(0, m, tnr::GraphSection::OverviewTelemetry);
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
    if (!isVisible()) return;
    PresentationScheduler::instance().request(this, [this] {
        if (dirty_ && isVisible()) { dirty_ = false; refresh(); }
    }, PresentationScheduler::Policy::Chart);
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
    const SessionData& data = model_->data();
    const auto section = tnr::GraphSection::OverviewTelemetry;
    const ChartWindow window = model_->effectiveChartWindow(section);
    const int selectedLap = model_->referenceLap(section);
    const ChartDomain domain = resolveChartDomain(data, window, selectedLap, now,
        model_->sectorBoundaries(), model_->chartPrimaryLap(now),
        model_->chartReferenceLap(window, selectedLap, now));
    setXRange(axXId_, domain.lower, domain.upper);
    setAxisDistanceMode(axXId_, domain.distance);
    syncAxisSessionMap(axXId_, domain.distance ? domain.primary : nullptr, domain.currentTime);
    if (!domain.ticks.isEmpty()) setAxisLabelMap(axXId_, domain.ticks, domain.tickLabels,
        chartWindowAccumulatesLaps(domain.window));
    else setAxisTimeTicker(axXId_, "%m:%s");
    setCursorModeKey(chartWindowKey(domain.window));
    setCursorSync(model_->cursorSync(), model_->secondaryVerticalCrosshair(),
                  model_->secondaryHorizontalCrosshair());

    const QString modeKey = chartWindowKey(domain.window);
    const QString primaryKey = QString("%1:%2:%3")
        .arg(modeKey)
        .arg(domain.primary ? domain.primary->lapNum : -1)
        .arg(domain.window == ChartWindow::StintLaps ? qRound64(domain.lower * 1000.0) : 0)
        + QString(":%1").arg(model_->playbackDataRevision());
    if (chartWindowIsTime(domain.window)) {
        const bool rebuild = dataModeKey_ != primaryKey || now < prevEndTime_ ||
                             std::abs(now - prevEndTime_) > 1.0f;
        if (rebuild) {
            clear(spId_); clear(rpId_); clear(erId_);
            clear(rSpId_); clear(rRpId_); clear(rErId_);
            lastAddedTime_ = domain.lower;
            lastAddedStsTime_ = domain.lower;
            dataModeKey_ = primaryKey;
        }
        auto lb = [](const auto& rows, float t) {
            return std::lower_bound(rows.begin(), rows.end(), t,
                [](const auto& row, float value) { return row.t < value; });
        };
        for (auto it = lb(data.telBuf, lastAddedTime_ + 0.0001f); it != data.telBuf.end(); ++it) {
            if (it->t > now) break;
            appendPoint(spId_, it->t, it->speed);
            appendPoint(rpId_, it->t, it->rpm);
            lastAddedTime_ = it->t;
        }
        for (auto it = lb(data.stsBuf, lastAddedStsTime_ + 0.0001f); it != data.stsBuf.end(); ++it) {
            if (it->t > now) break;
            appendPoint(erId_, it->t, it->ers);
            lastAddedStsTime_ = it->t;
        }
        trimBefore(spId_, domain.lower); trimBefore(rpId_, domain.lower); trimBefore(erId_, domain.lower);
        showReference(false);
    } else {
        const bool rebuild = dataModeKey_ != primaryKey || now < prevEndTime_ ||
                             std::abs(now - prevEndTime_) > 1.0f;
        if (rebuild) {
            QVector<double> tx, speed, rpm, sx, ers;
            const QVector<TelSample> tel = chartTelSamples(data, domain);
            const QVector<StsSample> sts = chartStatusSamples(data, domain);
            for (const TelSample& sample : tel) {
                const double key = domain.distance ? data.distanceAtTime(domain.primary, sample.t) : sample.t;
                if (!qIsFinite(key)) continue;
                tx.push_back(key); speed.push_back(sample.speed); rpm.push_back(sample.rpm);
                lastAddedTime_ = sample.t;
            }
            for (const StsSample& sample : sts) {
                const double key = domain.distance ? data.distanceAtTime(domain.primary, sample.t) : sample.t;
                if (!qIsFinite(key)) continue;
                sx.push_back(key); ers.push_back(sample.ers);
                lastAddedStsTime_ = sample.t;
            }
            setSeriesData(spId_, tx, speed); setSeriesData(rpId_, tx, rpm); setSeriesData(erId_, sx, ers);
            if (tel.isEmpty()) lastAddedTime_ = domain.lower;
            if (sts.isEmpty()) lastAddedStsTime_ = domain.lower;
            dataModeKey_ = primaryKey;
        } else {
            const QVector<TelSample>& tel = domain.distance && domain.primary
                ? domain.primary->tel : data.telBuf;
            const QVector<StsSample>& sts = domain.distance && domain.primary
                ? domain.primary->sts : data.stsBuf;
            auto telStart = std::lower_bound(tel.begin(), tel.end(), lastAddedTime_ + 0.0001f,
                [](const TelSample& sample, float value) { return sample.t < value; });
            for (auto it = telStart; it != tel.end(); ++it) {
                if (it->t > now) break;
                if (it->t < domain.lower) continue;
                const double key = domain.distance ? data.distanceAtTime(domain.primary, it->t) : it->t;
                if (!qIsFinite(key)) continue;
                appendPoint(spId_, key, it->speed); appendPoint(rpId_, key, it->rpm);
                lastAddedTime_ = it->t;
            }
            auto stsStart = std::lower_bound(sts.begin(), sts.end(), lastAddedStsTime_ + 0.0001f,
                [](const StsSample& sample, float value) { return sample.t < value; });
            for (auto it = stsStart; it != sts.end(); ++it) {
                if (it->t > now) break;
                if (it->t < domain.lower) continue;
                const double key = domain.distance ? data.distanceAtTime(domain.primary, it->t) : it->t;
                if (!qIsFinite(key)) continue;
                appendPoint(erId_, key, it->ers);
                lastAddedStsTime_ = it->t;
            }
        }
    }

    const QString referenceKey = domain.comparison && domain.reference
        ? QString("%1:%2:%3").arg(modeKey).arg(domain.reference->lapNum)
            .arg(model_->playbackDataRevision())
        : QString();
    if (referenceKey != referenceModeKey_) {
        QVector<double> rtx, rspeed, rrpm, rsx, rers;
        if (domain.comparison && domain.reference) {
        for (const TelSample& sample : domain.reference->tel) {
            const double key = projectReferenceTime(data, domain, sample.t);
            if (!qIsFinite(key)) continue;
            rtx.push_back(key); rspeed.push_back(sample.speed); rrpm.push_back(sample.rpm);
        }
        for (const StsSample& sample : domain.reference->sts) {
            const double key = projectReferenceTime(data, domain, sample.t);
            if (!qIsFinite(key)) continue;
            rsx.push_back(key); rers.push_back(sample.ers);
        }
        }
        setSeriesData(rSpId_, rtx, rspeed); setSeriesData(rRpId_, rtx, rrpm); setSeriesData(rErId_, rsx, rers);
        referenceModeKey_ = referenceKey;
    }
    setSeriesVisible(rSpId_, domain.comparison && seriesVisible(spId_));
    setSeriesVisible(rRpId_, domain.comparison && seriesVisible(rpId_));
    setSeriesVisible(rErId_, domain.comparison && seriesVisible(erId_));
    requestReplot();
    prevEndTime_ = now;
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
