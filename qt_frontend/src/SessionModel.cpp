#include "SessionModel.h"

#include <algorithm>
#include <utility>
#include <QSettings>
#include <QSet>

namespace {
constexpr uint32_t rowBit(uint8_t type) { return 1u << type; }
constexpr qsizetype kTrimChunk = 4096;
constexpr qsizetype kMaxRows = 750000;

template <typename T>
void mergeTimed(QVector<T>& target, QVector<T>&& incoming) {
    if (incoming.isEmpty()) return;
    if (target.isEmpty()) {
        target = std::move(incoming);
        if (target.size() > kMaxRows) target.remove(0, target.size() - kMaxRows);
        return;
    }

    // Playback/history producers already deliver each vector in time order.
    // The former append + stable_sort re-sorted the entire accumulated session
    // for every additive backfill. Merge the two sorted ranges linearly and let
    // the newer incoming value win at an identical timestamp.
    QVector<T> merged;
    merged.reserve(qMin<qsizetype>(kMaxRows, target.size() + incoming.size()));
    qsizetype a = 0;
    qsizetype b = 0;
    while (a < target.size() && b < incoming.size()) {
        if (target[a].t < incoming[b].t) {
            merged.push_back(std::move(target[a++]));
        } else if (incoming[b].t < target[a].t) {
            merged.push_back(std::move(incoming[b++]));
        } else {
            merged.push_back(std::move(incoming[b++]));
            ++a;
        }
    }
    while (a < target.size()) merged.push_back(std::move(target[a++]));
    while (b < incoming.size()) merged.push_back(std::move(incoming[b++]));
    target = std::move(merged);
    if (target.size() > kMaxRows) target.remove(0, target.size() - kMaxRows);
}
} // namespace

// ── SessionData: lap segmentation + buffering ───────────────────────────────

void SessionData::onTelemetry(float t, float speed, int rpm, int gear, float throttle, float brake, float steering) {
    latestTime = t;
    telBuf.push_back({ t, speed, rpm, gear, throttle, brake, steering });
    if (curLapNum >= 0) curLap.tel.push_back({ t, speed, rpm, gear, throttle, brake, steering });
}

void SessionData::onStatus(float t, float ers, float fuel_kg, float ice_kw, float mguk_kw, float mguk_harvest_j, float mguh_harvest_j,
                           int tyre_compound, int visual_compound, int tyre_age_laps) {
    const StsSample sample{ t, ers, fuel_kg, ice_kw, mguk_kw, mguk_harvest_j, mguh_harvest_j,
                            tyre_compound, visual_compound, tyre_age_laps };
    if (!stsBuf.isEmpty()) {
        const StsSample& previous = stsBuf.last();
        const bool compoundsValid = previous.tyre_compound > 0 && tyre_compound > 0;
        const bool compoundChanged = compoundsValid &&
            (previous.tyre_compound != tyre_compound || previous.visual_compound != visual_compound);
        const int ageDelta = tyre_age_laps - previous.tyre_age_laps;
        const bool usedSetFitted = ageDelta > 1 && t - previous.t < 30.0f;
        if (compoundChanged || ageDelta < 0 || usedSetFitted) currentStintStartTime = t;
    } else {
        currentStintStartTime = t;
    }
    stsBuf.push_back(sample);
    if (curLapNum >= 0) curLap.sts.push_back(sample);
}

void SessionData::onDamage(float t, float wearFl, float wearFr, float wearRl, float wearRr) {
    const DamageSample sample{ t, wearFl, wearFr, wearRl, wearRr };
    damageBuf.push_back(sample);
    if (curLapNum >= 0) curLap.damage.push_back(sample);
}

void SessionData::onMotion(float t, float g_lat, float g_long) {
    const MotionSample sample{ t, g_lat, g_long };
    motionBuf.push_back(sample);
    if (curLapNum >= 0) curLap.motion.push_back(sample);
}

void SessionData::onMotionEx(float t, float front_aero, float rear_aero) {
    const MotionExSample sample{ t, front_aero, rear_aero };
    motionExBuf.push_back(sample);
    if (curLapNum >= 0) curLap.motionEx.push_back(sample);
}

void SessionData::onTyre(float t,
                         float surfFl, float surfFr, float surfRl, float surfRr,
                         float innerFl, float innerFr, float innerRl, float innerRr,
                         float brakeFl, float brakeFr, float brakeRl, float brakeRr,
                         float wearFl,  float wearFr,  float wearRl,  float wearRr) {
    const TyreSample sample{ t,
        surfFl, surfFr, surfRl, surfRr,
        innerFl, innerFr, innerRl, innerRr,
        brakeFl, brakeFr, brakeRl, brakeRr,
        wearFl, wearFr, wearRl, wearRr };
    tyreBuf.push_back(sample);
    if (curLapNum >= 0) curLap.tyre.push_back(sample);
}

void SessionData::onLap(int lapNum, int currentLapMs, int lastLapMs, bool invalid,
                        int driverStatus, bool timedSession, float sessionTime,
                        float lapDistanceM, int sector, int pitStatus) {
    if (sessionTime >= 0) latestTime = qMax(latestTime, sessionTime);
    const bool garageAware = timedSession && driverStatus >= 0;
    if (garageAware && driverStatus != 1) {
        curLap = LapBlock{};
        curLapNum = -1;
        lapStartTime = latestTime;
        return;
    }
    if (curLapNum < 0) {
        // First lap seen: backtrack its start from the elapsed lap time.
        curLapNum    = lapNum;
        lapStartTime = currentLapMs > 0 ? latestTime - currentLapMs / 1000.0f : latestTime;
        curLap = LapBlock{};
        curLap.lapNum = lapNum;
        curLap.startSessionTime = lapStartTime;
        curLap.invalid = invalid;
    } else if (lapNum > curLapNum) {
        finalizeCurrentLap(lastLapMs);           // lastLapMs is the just-finished lap's time
        curLapNum    = lapNum;
        lapStartTime = currentLapMs > 0 ? latestTime - currentLapMs / 1000.0f : latestTime;
        curLap = LapBlock{};
        curLap.lapNum = lapNum;
        curLap.startSessionTime = lapStartTime;
    }
    if (curLapNum >= 0) {
        // Pit-lane distances can be negative and must not distort the progress map.
        const float distance = qMax(0.0f, lapDistanceM);
        curLap.progress.push_back({ latestTime, currentLapMs, distance, sector });
        if (pitStatus == 0) trackLengthM = qMax(trackLengthM, distance);
    }
    curLap.invalid = invalid;
}

void SessionData::finalizeCurrentLap(int lastLapMs) {
    if (curLapNum < 0) return;
    curLap.endSessionTime = latestTime;
    curLap.lapTimeMs = lastLapMs;
    laps.push_back(curLap);

    if (lastLapMs > 0 && lastLapMs < 300000 && lastLapMs < fastestLapMs) {
        fastestLapMs  = lastLapMs;
        fastestLapNum = curLap.lapNum;
    }
}

void SessionData::finalizeOpenLap() {
    // Push the trailing in-progress lap (e.g. end of a recording) with no
    // authoritative lap time. Used after a playback scan so every lap is queryable.
    if (curLapNum < 0 || curLap.tel.isEmpty()) return;
    curLap.endSessionTime = latestTime;
    laps.push_back(curLap);
    curLap = LapBlock{};
    curLapNum = -1;
}

void SessionData::onSessionReset(float newTime) {
    clear();
    latestTime = newTime;
}

void SessionData::truncateAfter(float newTime) {
    // In-game flashback/rewind: the game replays from an earlier point, so drop
    // every sample newer than the rewind target and keep the earlier history —
    // mirrors the Electron buffer filter (keep session_time < incoming) instead of
    // wiping the whole session. Buffers are time-ordered, so trim from the tail.
    auto cutTail = [newTime](auto& buf) {
        int n = buf.size();
        while (n > 0 && buf[n - 1].t >= newTime) --n;
        buf.remove(n, buf.size() - n);
    };
    cutTail(telBuf);
    cutTail(stsBuf);
    cutTail(motionBuf);
    cutTail(motionExBuf);
    cutTail(tyreBuf);
    cutTail(damageBuf);

    // Drop completed laps recorded at/after the rewind point.
    while (!laps.isEmpty() && laps.last().startSessionTime >= newTime)
        laps.removeLast();

    // Discard the in-progress lap; lap tracking re-initialises from the next lap
    // packet (matches resetting lapNum/lapStart on a backward step in Electron).
    curLap = LapBlock{};
    curLapNum = -1;
    lapStartTime = newTime;

    // Recompute the fastest lap over the laps that survived the rewind.
    fastestLapNum = -1;
    fastestLapMs  = INT_MAX;
    for (const LapBlock& l : laps)
        if (l.lapTimeMs > 0 && l.lapTimeMs < 300000 && l.lapTimeMs < fastestLapMs) {
            fastestLapMs  = l.lapTimeMs;
            fastestLapNum = l.lapNum;
    }

    latestTime = telBuf.isEmpty() ? newTime : telBuf.last().t;
}

void SessionData::clear() {
    telBuf.clear();
    stsBuf.clear();
    motionBuf.clear();
    motionExBuf.clear();
    tyreBuf.clear();
    damageBuf.clear();
    laps.clear();
    curLap = LapBlock{};
    curLapNum = -1;
    lapStartTime = 0;
    fastestLapNum = -1;
    fastestLapMs = INT_MAX;
    latestTime = 0;
    trackLengthM = 0;
    currentStintStartTime = 0;
}

void SessionData::trim() {
    if (!trimBuffers) return;
    // Match Electron's source-buffer retention exactly: keep the complete
    // session until 750,000 rows plus one slack chunk, then compact back to the
    // hard cap. The former 600-second cutoff silently discarded early All Laps
    // history even though Electron deliberately retains it.
    auto prune = [](auto& rows) {
        if (rows.size() > kMaxRows + kTrimChunk)
            rows.remove(0, rows.size() - kMaxRows);
    };
    prune(telBuf);
    prune(stsBuf);
    prune(motionBuf);
    prune(motionExBuf);
    prune(tyreBuf);
    prune(damageBuf);
}

void SessionData::finishIngestBatch() { trim(); }

const LapBlock* SessionData::lapByNum(int n) const {
    for (const LapBlock& l : laps) if (l.lapNum == n) return &l;
    if (curLapNum == n) return &curLap;
    return nullptr;
}

LapBlock* SessionData::lapByNum(int n) {
    for (LapBlock& l : laps) if (l.lapNum == n) return &l;
    if (curLapNum == n) return &curLap;
    return nullptr;
}

const LapBlock* SessionData::lapAtTime(float t) const {
    auto it = std::upper_bound(laps.begin(), laps.end(), t,
        [](float value, const LapBlock& lap) { return value < lap.startSessionTime; });
    if (it == laps.begin()) return nullptr;
    --it;
    if (t >= it->startSessionTime && t <= it->endSessionTime) return &*it;
    return nullptr;
}

LapBlock* SessionData::lapAtTime(float t) {
    auto it = std::upper_bound(laps.begin(), laps.end(), t,
        [](float value, const LapBlock& lap) { return value < lap.startSessionTime; });
    if (it == laps.begin()) return nullptr;
    --it;
    if (t >= it->startSessionTime && t <= it->endSessionTime) return &*it;
    return nullptr;
}

const LapBlock* SessionData::currentLapAt(float t) const {
    if (const LapBlock* lap = lapAtTime(t)) return lap;
    if (curLapNum >= 0 && t >= curLap.startSessionTime) return &curLap;
    return laps.isEmpty() ? nullptr : &laps.last();
}

const LapBlock* SessionData::previousLap(const LapBlock* current) const {
    if (!current) return nullptr;
    return lapByNum(current->lapNum - 1);
}

double SessionData::distanceAtTime(const LapBlock* lap, float t) const {
    if (!lap || lap->progress.isEmpty()) return qQNaN();
    const auto& p = lap->progress;
    auto it = std::lower_bound(p.begin(), p.end(), t,
        [](const LapProgressSample& sample, float key) { return sample.t < key; });
    if (it == p.begin()) return it->distanceM;
    // Match Electron's lap-coordinate interpolation: rows beyond the published
    // progress endpoint are not part of the current lap yet. This matters during
    // playback, where an on-demand completed-lap block may extend past the cursor.
    if (it == p.end()) return t <= p.last().t ? p.last().distanceM : qQNaN();
    const auto& after = *it;
    const auto& before = *(it - 1);
    const double span = after.t - before.t;
    return span > 0 ? before.distanceM + (after.distanceM - before.distanceM) * (t - before.t) / span
                    : after.distanceM;
}

double SessionData::timeAtDistance(const LapBlock* lap, double distance) const {
    if (!lap || lap->progress.isEmpty()) return qQNaN();
    const auto& p = lap->progress;
    auto it = std::lower_bound(p.begin(), p.end(), distance,
        [](const LapProgressSample& sample, double key) { return sample.distanceM < key; });
    if (it == p.begin()) return it->t;
    if (it == p.end()) return p.last().t;
    const auto& after = *it;
    const auto& before = *(it - 1);
    const double span = after.distanceM - before.distanceM;
    return span > 0 ? before.t + (after.t - before.t) * (distance - before.distanceM) / span
                    : after.t;
}

QVector<LapProgressSample> SessionData::sectorSplits(const LapBlock* lap) const {
    QVector<LapProgressSample> result;
    if (!lap) return result;
    int lastSector = lap->progress.isEmpty() ? 0 : lap->progress.first().sector;
    for (const auto& point : lap->progress) {
        if (point.sector > lastSector && point.sector <= 2) result.push_back(point);
        lastSector = point.sector;
    }
    return result;
}

// ── SessionModel: QObject wrapper + per-frame coalescing ────────────────────

SessionModel::SessionModel(QObject* parent) : QObject(parent) {
    const int count = static_cast<int>(tnr::GraphSection::Count_);
    windowOverrides_.fill(-1, count);
    referenceLaps_.fill(0, count);
    dynamicYAxes_.fill(false, count);
    QSettings s("TrackNRace", "NativeRecorder");
    globalWindow_ = chartWindowFromKey(s.value("ui/chartWindow/global", "30").toString());
    globalReferenceLap_ = s.value("ui/chartReferenceLap/global", 0).toInt();
    sectorBoundaries_ = s.value("ui/chartSectorBoundaries", false).toBool();
    cursorSync_ = s.value("ui/chartCursorSync", false).toBool();
    secondaryVertical_ = s.value("ui/chartSecondaryVertical", true).toBool();
    secondaryHorizontal_ = s.value("ui/chartSecondaryHorizontal", false).toBool();
    for (int i = 0; i < count; ++i) {
        const auto section = static_cast<tnr::GraphSection>(i);
        const QString override = s.value(chartWindowOverrideKey(section)).toString();
        if (!override.isEmpty()) windowOverrides_[i] = static_cast<int>(chartWindowFromKey(override));
        referenceLaps_[i] = s.value(chartReferenceLapKey(section), 0).toInt();
        dynamicYAxes_[i] = s.value(chartYAxisKey(section), false).toBool();
    }
}

// Emits telemetryAppended()/tyreAppended() at most once per event-loop pass. On the
// GUI thread that is once per arriving packet, so charts refresh at the true data
// rate (20..60 Hz) instead of a fixed clock, while the several ingest setters fired
// by one composite packet still collapse into a single emission.
void SessionModel::scheduleFlush() {
    if (ingestBatchDepth_ > 0) return;
    d_.finishIngestBatch();
    if (!telemetryDirty_ && !tyreDirty_) return;
    if (!flushActive_ || flushScheduled_) return;
    flushScheduled_ = true;
    QTimer::singleShot(0, this, [this] {
        flushScheduled_ = false;
        if (telemetryDirty_) { telemetryDirty_ = false; emit telemetryAppended(); }
        if (tyreDirty_)      { tyreDirty_      = false; emit tyreAppended(); }
    });
}

void SessionModel::setLiveFlushActive(bool on) {
    if (on) {
        if (!flushActive_) {
            flushActive_    = true;
            telemetryDirty_ = true;   // force one flush so charts catch up on the data ingested while paused
            tyreDirty_      = true;
            scheduleFlush();
        }
    } else {
        flushActive_ = false;         // ingest keeps running; samples accumulate until resumed
    }
}

void SessionModel::onTelemetry(float t, float speed, int rpm, int gear, float throttle, float brake, float steering) {
    if (playbackMode_ && !(playbackRequestedHistoryMask_ & rowBit(1))) {
        d_.latestTime = qMax(d_.latestTime, t);
        return;
    }
    d_.onTelemetry(t, speed, rpm, gear, throttle, brake, steering);
    if (playbackMode_) {
        if (LapBlock* lap = d_.lapAtTime(t);
            lap && (playbackActiveLapMasks_.value(lap->lapNum) & rowBit(1)))
            lap->tel.push_back({t, speed, rpm, gear, throttle, brake, steering});
    }
    telemetryDirty_ = true;
    scheduleFlush();
}

void SessionModel::onStatus(float t, float ers, float fuel_kg, float ice_kw, float mguk_kw, float mguk_harvest_j, float mguh_harvest_j,
                            int tyre_compound, int visual_compound, int tyre_age_laps) {
    if (playbackMode_ && !(playbackRequestedHistoryMask_ & rowBit(2))) {
        d_.latestTime = qMax(d_.latestTime, t);
        return;
    }
    d_.onStatus(t, ers, fuel_kg, ice_kw, mguk_kw, mguk_harvest_j, mguh_harvest_j, tyre_compound, visual_compound, tyre_age_laps);
    if (playbackMode_) {
        if (LapBlock* lap = d_.lapAtTime(t);
            lap && (playbackActiveLapMasks_.value(lap->lapNum) & rowBit(2)))
            lap->sts.push_back({t, ers, fuel_kg, ice_kw, mguk_kw, mguk_harvest_j,
                                mguh_harvest_j, tyre_compound, visual_compound,
                                tyre_age_laps});
    }
    telemetryDirty_ = true;
    scheduleFlush();
}

void SessionModel::onDamage(float t, float wearFl, float wearFr, float wearRl, float wearRr) {
    if (playbackMode_ && !(playbackRequestedHistoryMask_ & rowBit(3))) {
        d_.latestTime = qMax(d_.latestTime, t);
        return;
    }
    d_.onDamage(t, wearFl, wearFr, wearRl, wearRr);
    if (playbackMode_) {
        if (LapBlock* lap = d_.lapAtTime(t);
            lap && (playbackActiveLapMasks_.value(lap->lapNum) & rowBit(3)))
            lap->damage.push_back({t, wearFl, wearFr, wearRl, wearRr});
    }
    telemetryDirty_ = true;
    scheduleFlush();
}

void SessionModel::onMotion(float t, float g_lat, float g_long) {
    if (playbackMode_ && !(playbackRequestedHistoryMask_ & rowBit(11))) {
        d_.latestTime = qMax(d_.latestTime, t);
        return;
    }
    d_.onMotion(t, g_lat, g_long);
    if (playbackMode_) {
        if (LapBlock* lap = d_.lapAtTime(t);
            lap && (playbackActiveLapMasks_.value(lap->lapNum) & rowBit(11)))
            lap->motion.push_back({t, g_lat, g_long});
    }
    telemetryDirty_ = true;
    scheduleFlush();
}

void SessionModel::onMotionEx(float t, float front_aero, float rear_aero) {
    if (playbackMode_ && !(playbackRequestedHistoryMask_ & rowBit(12))) {
        d_.latestTime = qMax(d_.latestTime, t);
        return;
    }
    d_.onMotionEx(t, front_aero, rear_aero);
    if (playbackMode_) {
        if (LapBlock* lap = d_.lapAtTime(t);
            lap && (playbackActiveLapMasks_.value(lap->lapNum) & rowBit(12)))
            lap->motionEx.push_back({t, front_aero, rear_aero});
    }
    telemetryDirty_ = true;
    scheduleFlush();
}

void SessionModel::onTyre(float t,
                          float surfFl, float surfFr, float surfRl, float surfRr,
                          float innerFl, float innerFr, float innerRl, float innerRr,
                          float brakeFl, float brakeFr, float brakeRl, float brakeRr,
                          float wearFl,  float wearFr,  float wearRl,  float wearRr) {
    if (playbackMode_ && !(playbackRequestedHistoryMask_ & rowBit(1))) {
        d_.latestTime = qMax(d_.latestTime, t);
        return;
    }
    d_.onTyre(t,
              surfFl, surfFr, surfRl, surfRr,
              innerFl, innerFr, innerRl, innerRr,
              brakeFl, brakeFr, brakeRl, brakeRr,
              wearFl, wearFr, wearRl, wearRr);
    if (playbackMode_) {
        if (LapBlock* lap = d_.lapAtTime(t);
            lap && (playbackActiveLapMasks_.value(lap->lapNum) & rowBit(1)))
            lap->tyre.push_back({t, surfFl, surfFr, surfRl, surfRr,
                                 innerFl, innerFr, innerRl, innerRr,
                                 brakeFl, brakeFr, brakeRl, brakeRr,
                                 wearFl, wearFr, wearRl, wearRr});
    }
    tyreDirty_ = true;
    scheduleFlush();
}

void SessionModel::onLap(int lapNum, int currentLapMs, int lastLapMs, bool invalid,
                         int driverStatus, bool timedSession, float sessionTime,
                         float lapDistanceM, int sector, int pitStatus) {
    if (playbackMode_ && !playbackCatalogLaps_.isEmpty()) {
        d_.latestTime = qMax(d_.latestTime, sessionTime);
        if (LapBlock* lap = d_.lapByNum(lapNum)) {
            lap->invalid = invalid;
            // A streamed lap row is one point, not proof that a whole indexed
            // lap family was loaded. Only extend a current-lap history that an
            // actual seek/window response already installed.
            if (playbackActiveLapMasks_.value(lapNum) & rowBit(4)) {
                lap->progress.push_back({sessionTime, currentLapMs,
                                         qMax(0.0f, lapDistanceM), sector});
                if (pitStatus == 0) d_.trackLengthM = qMax(d_.trackLengthM, lapDistanceM);
            }
        }
        telemetryDirty_ = true;
        scheduleFlush();
        return;
    }
    const int before = d_.laps.size();
    d_.onLap(lapNum, currentLapMs, lastLapMs, invalid, driverStatus, timedSession,
             sessionTime, lapDistanceM, sector, pitStatus);
    if (d_.laps.size() != before) emit lapsChanged();
}

void SessionModel::beginIngestBatch() { ++ingestBatchDepth_; }

void SessionModel::endIngestBatch() {
    if (ingestBatchDepth_ <= 0) return;
    if (--ingestBatchDepth_ != 0) return;
    d_.finishIngestBatch();
    scheduleFlush();
}

ChartWindow SessionModel::effectiveChartWindow(tnr::GraphSection section) const {
    const int i = static_cast<int>(section);
    ChartWindow result = i >= 0 && i < windowOverrides_.size() && windowOverrides_[i] >= 0
        ? static_cast<ChartWindow>(windowOverrides_[i]) : globalWindow_;
    if (result == ChartWindow::SelectedLap && !playbackMode_) return ChartWindow::Seconds30;
    return result;
}

void SessionModel::setGlobalChartWindow(ChartWindow window) {
    bool changed = globalWindow_ != window;
    globalWindow_ = window;
    QSettings settings("TrackNRace", "NativeRecorder");
    settings.setValue("ui/chartWindow/global", chartWindowKey(window));
    for (int i = 0; i < windowOverrides_.size(); ++i) {
        const auto section = static_cast<tnr::GraphSection>(i);
        if (windowOverrides_[i] >= 0) changed = true;
        if (referenceLaps_[i] > 0) changed = true;
        windowOverrides_[i] = -1;
        referenceLaps_[i] = 0;
        settings.remove(chartWindowOverrideKey(section));
        settings.remove(chartReferenceLapKey(section));
    }
    if (changed) emit chartConfigurationChanged();
}

void SessionModel::setChartWindow(tnr::GraphSection section, ChartWindow window) {
    const int i = static_cast<int>(section);
    if (i < 0 || i >= windowOverrides_.size()) return;
    const int value = window == globalWindow_ ? -1 : static_cast<int>(window);
    if (windowOverrides_[i] == value) return;
    windowOverrides_[i] = value;
    QSettings s("TrackNRace", "NativeRecorder");
    if (value < 0) s.remove(chartWindowOverrideKey(section));
    else s.setValue(chartWindowOverrideKey(section), chartWindowKey(window));
    emit chartConfigurationChanged();
}

int SessionModel::referenceLap(tnr::GraphSection section) const {
    const int i = static_cast<int>(section);
    if (i < 0 || i >= referenceLaps_.size()) return globalReferenceLap_;
    return referenceLaps_[i] > 0 ? referenceLaps_[i] : globalReferenceLap_;
}

void SessionModel::setGlobalReferenceLap(int lapNum) {
    bool changed = globalReferenceLap_ != lapNum;
    globalReferenceLap_ = lapNum;
    QSettings settings("TrackNRace", "NativeRecorder");
    settings.setValue("ui/chartReferenceLap/global", lapNum);
    for (int i = 0; i < referenceLaps_.size(); ++i) {
        if (referenceLaps_[i] > 0) changed = true;
        referenceLaps_[i] = 0;
        settings.remove(chartReferenceLapKey(static_cast<tnr::GraphSection>(i)));
    }
    if (changed) emit chartConfigurationChanged();
}

void SessionModel::setReferenceLap(tnr::GraphSection section, int lapNum) {
    const int i = static_cast<int>(section);
    if (i < 0 || i >= referenceLaps_.size()) return;
    const int value = lapNum == globalReferenceLap_ ? 0 : lapNum;
    if (referenceLaps_[i] == value) return;
    referenceLaps_[i] = value;
    QSettings settings("TrackNRace", "NativeRecorder");
    if (value > 0) settings.setValue(chartReferenceLapKey(section), value);
    else settings.remove(chartReferenceLapKey(section));
    emit chartConfigurationChanged();
}

void SessionModel::setPlaybackMode(bool on) {
    if (playbackMode_ == on) return;
    playbackMode_ = on;
    emit chartConfigurationChanged();
}

bool SessionModel::lapCoordinatesAvailable() const {
    if (!playbackMode_) return true;
    if (playbackLapDistanceAvailable_ && d_.trackLengthM > 0.0f) return true;
    if (d_.trackLengthM <= 0.0f) return false;
    for (const LapBlock& lap : d_.laps)
        if (!lap.progress.isEmpty() && lap.progress.last().distanceM > 0.0f) return true;
    return d_.curLapNum >= 0 && !d_.curLap.progress.isEmpty() &&
           d_.curLap.progress.last().distanceM > 0.0f;
}

void SessionModel::setSectorBoundaries(bool on) {
    if (sectorBoundaries_ == on) return;
    sectorBoundaries_ = on;
    QSettings("TrackNRace", "NativeRecorder").setValue("ui/chartSectorBoundaries", on);
    emit chartConfigurationChanged();
}

void SessionModel::setCursorSync(bool on) {
    if (cursorSync_ == on) return;
    cursorSync_ = on;
    QSettings("TrackNRace", "NativeRecorder").setValue("ui/chartCursorSync", on);
    emit chartConfigurationChanged();
}

void SessionModel::setSecondaryCrosshairs(bool vertical, bool horizontal) {
    secondaryVertical_ = vertical;
    secondaryHorizontal_ = horizontal;
    QSettings s("TrackNRace", "NativeRecorder");
    s.setValue("ui/chartSecondaryVertical", vertical);
    s.setValue("ui/chartSecondaryHorizontal", horizontal);
    emit chartConfigurationChanged();
}

bool SessionModel::dynamicYAxis(tnr::GraphSection section) const {
    const int i = static_cast<int>(section);
    return i >= 0 && i < dynamicYAxes_.size() && dynamicYAxes_[i];
}

void SessionModel::setDynamicYAxis(tnr::GraphSection section, bool dynamic) {
    const int i = static_cast<int>(section);
    if (i < 0 || i >= dynamicYAxes_.size() || dynamicYAxes_[i] == dynamic) return;
    dynamicYAxes_[i] = dynamic;
    QSettings("TrackNRace", "NativeRecorder").setValue(chartYAxisKey(section), dynamic);
    emit chartConfigurationChanged();
}

void SessionModel::onSessionReset(float newTime) {
    if (playbackMode_) return;
    d_.onSessionReset(newTime);
    emit wasReset();
    emit lapsChanged();
}

void SessionModel::truncateAfter(float newTime) {
    if (playbackMode_) return;
    d_.truncateAfter(newTime);
    // Charts re-query the (now shorter) buffers; lap selectors re-read the laps.
    emit lapsChanged();
    emit telemetryAppended();
    emit tyreAppended();
}

void SessionModel::clear() {
    d_.clear();
    playbackCatalogLaps_.clear();
    playbackActiveLapMasks_.clear();
    playbackLapDataCache_.clear();
    playbackLapDataMasks_.clear();
    playbackLapLru_.clear();
    playbackHistoryMask_ = 0;
    playbackRequestedHistoryMask_ = 0;
    ++playbackDataRevision_;
    playbackLapDistanceAvailable_ = false;
    emit wasReset();
    emit lapsChanged();
}

void SessionModel::setPlaybackCatalog(const tnrp::PlaybackLapBlocksRow& catalog) {
    d_.clear();
    d_.trimBuffers = true;
    d_.fastestLapNum = catalog.fastestLapNum;
    d_.trackLengthM = static_cast<float>(catalog.trackLengthM);
    playbackLapDistanceAvailable_ = catalog.lapDistanceAvailable;

    QHash<int, int> lapTimes;
    for (const auto& lap : catalog.laps) lapTimes.insert(lap.lapNum, lap.lapTimeMs);
    d_.laps.reserve(static_cast<qsizetype>(catalog.blocks.size()));
    for (const auto& source : catalog.blocks) {
        LapBlock lap;
        lap.lapNum = source.lapNum;
        lap.startSessionTime = source.startSessionTime;
        lap.endSessionTime = source.endSessionTime;
        lap.lapTimeMs = lapTimes.value(source.lapNum, 0);
        lap.tel.reserve(static_cast<qsizetype>(source.telemetry.size()));
        for (const auto& point : source.telemetry)
            lap.tel.push_back({point.session_time, static_cast<float>(point.speed_kph),
                               point.rpm, 0, 0.0f, 0.0f, 0.0f});
        lap.sts.reserve(static_cast<qsizetype>(source.statusHistory.size()));
        for (const auto& point : source.statusHistory)
            lap.sts.push_back({point.session_time, static_cast<float>(point.ers_pct),
                               0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                               point.tyre_compound, point.visual_compound, 0});
        d_.laps.push_back(std::move(lap));
    }
    std::sort(d_.laps.begin(), d_.laps.end(), [](const LapBlock& a, const LapBlock& b) {
        return a.startSessionTime < b.startSessionTime;
    });
    d_.latestTime = d_.laps.isEmpty() ? 0.0f : d_.laps.first().startSessionTime;
    d_.fastestLapMs = INT_MAX;
    for (const LapBlock& lap : d_.laps)
        if (lap.lapNum == d_.fastestLapNum && lap.lapTimeMs > 0)
            d_.fastestLapMs = lap.lapTimeMs;

    // Electron resets an invalid/empty selected-lap value to Lap 1 (or the
    // first catalog lap) as soon as the asynchronous catalog arrives. Do the
    // same before any selector refresh or data request observes the model.
    auto catalogHasLap = [this](int lapNum) {
        return std::any_of(d_.laps.cbegin(), d_.laps.cend(), [lapNum](const LapBlock& lap) {
            return lap.lapNum == lapNum;
        });
    };
    const int defaultLap = catalogHasLap(1) ? 1 : (d_.laps.isEmpty() ? 0 : d_.laps.first().lapNum);
    bool referenceChanged = false;
    QSettings settings("TrackNRace", "NativeRecorder");
    if (!catalogHasLap(globalReferenceLap_)) {
        globalReferenceLap_ = defaultLap;
        settings.setValue("ui/chartReferenceLap/global", globalReferenceLap_);
        referenceChanged = true;
    }
    for (int i = 0; i < referenceLaps_.size(); ++i) {
        if (referenceLaps_[i] <= 0 || catalogHasLap(referenceLaps_[i])) continue;
        referenceLaps_[i] = 0;
        settings.remove(chartReferenceLapKey(static_cast<tnr::GraphSection>(i)));
        referenceChanged = true;
    }

    playbackCatalogLaps_ = d_.laps;
    playbackActiveLapMasks_.clear();
    playbackLapDataCache_.clear();
    playbackLapDataMasks_.clear();
    playbackLapLru_.clear();
    playbackHistoryMask_ = 0;
    ++playbackDataRevision_;
    telemetryDirty_ = false;
    tyreDirty_      = false;
    emit lapsChanged();
    emit telemetryAppended();
    emit tyreAppended();
    if (referenceChanged) emit chartConfigurationChanged();
}

void SessionModel::touchPlaybackLap(int lapNum) {
    if (lapNum <= 0 || !playbackLapDataMasks_.contains(lapNum)) return;
    playbackLapLru_.removeAll(lapNum);
    playbackLapLru_.push_back(lapNum);
    while (playbackLapLru_.size() > 6) {
        const int evicted = playbackLapLru_.takeFirst();
        playbackLapDataCache_.remove(evicted);
        playbackLapDataMasks_.remove(evicted);
    }
}

void SessionModel::installPlaybackHistory(SessionData&& incoming,
                                          QVector<LapProgressSample>&& progress,
                                          QVector<LapBlock>&& lapDetails,
                                          bool authoritative,
                                          bool isolatedLapRequest,
                                          uint32_t rowTypeMask,
                                          float historyStart,
                                          int requestedLapNum) {
    Q_UNUSED(historyStart);
    Q_UNUSED(progress);
    const uint32_t payloadMask = rowTypeMask;
    const uint32_t activeMask = payloadMask & playbackRequestedHistoryMask_;
    if (authoritative) {
        d_.laps = playbackCatalogLaps_;
        d_.curLap = {};
        d_.curLapNum = -1;
        playbackActiveLapMasks_.clear();
    }

    // Indexed lap reads are an independent cache in Electron. Never merge them
    // into the active timeline or catalog: a completed reference lap may contain
    // future rows relative to the playhead, and a seek must not invalidate it.
    if (isolatedLapRequest) {
        bool installedAny = false;
        for (LapBlock& detail : lapDetails) {
            installedAny = true;
            auto cachedIt = playbackLapDataCache_.find(detail.lapNum);
            if (cachedIt == playbackLapDataCache_.end()) {
                LapBlock cached;
                if (const LapBlock* meta = d_.lapByNum(detail.lapNum)) {
                    cached.lapNum = meta->lapNum;
                    cached.startSessionTime = meta->startSessionTime;
                    cached.endSessionTime = meta->endSessionTime;
                    cached.lapTimeMs = meta->lapTimeMs;
                    cached.invalid = meta->invalid;
                } else {
                    cached.lapNum = detail.lapNum;
                    cached.startSessionTime = detail.startSessionTime;
                    cached.endSessionTime = detail.endSessionTime;
                }
                cachedIt = playbackLapDataCache_.insert(detail.lapNum, std::move(cached));
            }
            LapBlock& cached = cachedIt.value();
            uint32_t& installedMask = playbackLapDataMasks_[detail.lapNum];
            auto install = [&installedMask](uint32_t bit, auto& target, auto& source) {
                if (!(installedMask & bit)) target = std::move(source);
                else mergeTimed(target, std::move(source));
                installedMask |= bit;
            };
            if (payloadMask & rowBit(1)) {
                install(rowBit(1), cached.tel, detail.tel);
                if (cached.tyre.isEmpty()) cached.tyre = std::move(detail.tyre);
                else mergeTimed(cached.tyre, std::move(detail.tyre));
            }
            if (payloadMask & rowBit(2)) install(rowBit(2), cached.sts, detail.sts);
            if (payloadMask & rowBit(3)) install(rowBit(3), cached.damage, detail.damage);
            if (payloadMask & rowBit(11)) install(rowBit(11), cached.motion, detail.motion);
            if (payloadMask & rowBit(12)) install(rowBit(12), cached.motionEx, detail.motionEx);
            if (payloadMask & rowBit(4)) {
                install(rowBit(4), cached.progress, detail.progress);
                for (const LapProgressSample& sample : cached.progress)
                    d_.trackLengthM = qMax(d_.trackLengthM, sample.distanceM);
            }
            touchPlaybackLap(detail.lapNum);
        }
        if (installedAny) ++playbackDataRevision_;
        telemetryDirty_ = false;
        tyreDirty_ = false;
        emit telemetryAppended();
        emit tyreAppended();
        return;
    }

    // Seek/window history is the active timeline. The worker already partitioned
    // it by immutable catalog ranges; only these vectors back the current-lap
    // fallback while an indexed current-lap payload is still in flight.
    for (LapBlock& detail : lapDetails) {
        LapBlock* lap = d_.lapByNum(detail.lapNum);
        if (!lap) continue;
        uint32_t& installedMask = playbackActiveLapMasks_[lap->lapNum];
        auto install = [authoritative, &installedMask](uint32_t bit, auto& target, auto& source) {
            if (authoritative || !(installedMask & bit)) target = std::move(source);
            else mergeTimed(target, std::move(source));
            installedMask |= bit;
        };
        if (activeMask & rowBit(1)) {
            install(rowBit(1), lap->tel, detail.tel);
            if (authoritative || lap->tyre.isEmpty()) lap->tyre = std::move(detail.tyre);
            else mergeTimed(lap->tyre, std::move(detail.tyre));
        }
        if (activeMask & rowBit(2)) install(rowBit(2), lap->sts, detail.sts);
        if (activeMask & rowBit(3)) install(rowBit(3), lap->damage, detail.damage);
        if (activeMask & rowBit(11)) install(rowBit(11), lap->motion, detail.motion);
        if (activeMask & rowBit(12)) install(rowBit(12), lap->motionEx, detail.motionEx);
        if (activeMask & rowBit(4)) {
            install(rowBit(4), lap->progress, detail.progress);
            for (const LapProgressSample& sample : lap->progress)
                d_.trackLengthM = qMax(d_.trackLengthM, sample.distanceM);
        }
    }

    if (authoritative) {
        d_.telBuf = activeMask & rowBit(1) ? std::move(incoming.telBuf) : QVector<TelSample>{};
        d_.tyreBuf = activeMask & rowBit(1) ? std::move(incoming.tyreBuf) : QVector<TyreSample>{};
        d_.stsBuf = activeMask & rowBit(2) ? std::move(incoming.stsBuf) : QVector<StsSample>{};
        d_.damageBuf = activeMask & rowBit(3) ? std::move(incoming.damageBuf) : QVector<DamageSample>{};
        d_.motionBuf = activeMask & rowBit(11) ? std::move(incoming.motionBuf) : QVector<MotionSample>{};
        d_.motionExBuf = activeMask & rowBit(12) ? std::move(incoming.motionExBuf) : QVector<MotionExSample>{};
        playbackHistoryMask_ = activeMask;
    } else {
        if (activeMask & rowBit(1)) {
            mergeTimed(d_.telBuf, std::move(incoming.telBuf));
            mergeTimed(d_.tyreBuf, std::move(incoming.tyreBuf));
        }
        if (activeMask & rowBit(2)) mergeTimed(d_.stsBuf, std::move(incoming.stsBuf));
        if (activeMask & rowBit(3)) mergeTimed(d_.damageBuf, std::move(incoming.damageBuf));
        if (activeMask & rowBit(11)) mergeTimed(d_.motionBuf, std::move(incoming.motionBuf));
        if (activeMask & rowBit(12)) mergeTimed(d_.motionExBuf, std::move(incoming.motionExBuf));
        playbackHistoryMask_ |= activeMask;
    }
    d_.trimBuffers = true;
    d_.latestTime = std::max(d_.latestTime, incoming.latestTime);
    ++playbackDataRevision_;
    Q_UNUSED(requestedLapNum);
    telemetryDirty_ = false;
    tyreDirty_ = false;
    // The catalog did not change. Rebuilding every lap selector here caused a
    // selected-lap response to retrigger selection state while it was being
    // installed. Charts already refresh from the family signals below.
    emit telemetryAppended();
    emit tyreAppended();
}

void SessionModel::retainPlaybackHistoryMask(uint32_t rowTypeMask) {
    const uint32_t removed = (playbackHistoryMask_ | playbackRequestedHistoryMask_) & ~rowTypeMask;
    playbackRequestedHistoryMask_ = rowTypeMask;
    playbackHistoryMask_ &= rowTypeMask;
    if (!removed) return;
    if (removed & rowBit(1)) { d_.telBuf.clear(); d_.tyreBuf.clear(); }
    if (removed & rowBit(2)) d_.stsBuf.clear();
    if (removed & rowBit(3)) d_.damageBuf.clear();
    if (removed & rowBit(11)) d_.motionBuf.clear();
    if (removed & rowBit(12)) d_.motionExBuf.clear();

    for (LapBlock& lap : d_.laps) {
        const LapBlock* baseline = nullptr;
        for (const LapBlock& candidate : playbackCatalogLaps_)
            if (candidate.lapNum == lap.lapNum) { baseline = &candidate; break; }
        if (removed & rowBit(1)) {
            lap.tel = baseline ? baseline->tel : QVector<TelSample>{};
            lap.tyre.clear();
        }
        if (removed & rowBit(2)) lap.sts = baseline ? baseline->sts : QVector<StsSample>{};
        if (removed & rowBit(3)) lap.damage.clear();
        if (removed & rowBit(4)) lap.progress.clear();
        if (removed & rowBit(11)) lap.motion.clear();
        if (removed & rowBit(12)) lap.motionEx.clear();
        playbackActiveLapMasks_[lap.lapNum] &= ~removed;
        if (playbackActiveLapMasks_.value(lap.lapNum) == 0)
            playbackActiveLapMasks_.remove(lap.lapNum);
    }
    ++playbackDataRevision_;
    emit telemetryAppended();
    emit tyreAppended();
}

uint32_t SessionModel::missingPlaybackLapMask(int lapNum, uint32_t rowTypeMask) const {
    return rowTypeMask & ~playbackLapDataMasks_.value(lapNum, 0u);
}

const LapBlock* SessionModel::playbackLapData(int lapNum) const {
    const auto it = playbackLapDataCache_.constFind(lapNum);
    return it == playbackLapDataCache_.cend() ? nullptr : &it.value();
}

const LapBlock* SessionModel::chartPrimaryLap(float sessionTime) const {
    if (!playbackMode_)
        return d_.curLapNum >= 0 ? &d_.curLap : d_.currentLapAt(sessionTime);
    const LapBlock* catalogLap = d_.currentLapAt(sessionTime);
    if (!catalogLap) return nullptr;
    if (const LapBlock* cached = playbackLapData(catalogLap->lapNum)) return cached;
    return catalogLap;
}

const LapBlock* SessionModel::chartReferenceLap(ChartWindow window, int selectedLap,
                                                 float sessionTime) const {
    const LapBlock* current = d_.currentLapAt(sessionTime);
    int lapNum = 0;
    if (window == ChartWindow::PreviousLap && current) lapNum = current->lapNum - 1;
    else if (window == ChartWindow::FastestLap) lapNum = d_.fastestLapNum;
    else if (window == ChartWindow::SelectedLap) lapNum = selectedLap;
    if (lapNum <= 0) return nullptr;
    return playbackMode_ ? playbackLapData(lapNum) : d_.lapByNum(lapNum);
}
