#include "SessionModel.h"

#include <algorithm>

// ── SessionData: lap segmentation + buffering ───────────────────────────────

void SessionData::onTelemetry(float t, float speed, int rpm, int gear, float throttle, float brake, float steering) {
    latestTime = t;
    telBuf.push_back({ t, speed, rpm, gear, throttle, brake, steering });
    if (curLapNum >= 0) curLap.tel.push_back({ t, speed, rpm, gear, throttle, brake, steering });
    trim();
}

void SessionData::onStatus(float t, float ers, float fuel_kg, float ice_kw, float mguk_kw, float mguk_harvest_j, float mguh_harvest_j) {
    stsBuf.push_back({ t, ers, fuel_kg, ice_kw, mguk_kw, mguk_harvest_j, mguh_harvest_j });
    if (curLapNum >= 0) curLap.sts.push_back({ t, ers, fuel_kg, ice_kw, mguk_kw, mguk_harvest_j, mguh_harvest_j });
    trim();
}

void SessionData::onMotion(float t, float g_lat, float g_long) {
    motionBuf.push_back({ t, g_lat, g_long });
    trim();
}

void SessionData::onMotionEx(float t, float front_aero, float rear_aero) {
    motionExBuf.push_back({ t, front_aero, rear_aero });
    trim();
}

void SessionData::onLap(int lapNum, int currentLapMs, int lastLapMs, bool invalid) {
    if (curLapNum < 0) {
        // First lap seen: backtrack its start from the elapsed lap time.
        curLapNum    = lapNum;
        lapStartTime = currentLapMs > 0 ? latestTime - currentLapMs / 1000.0f : latestTime;
        curLap = LapBlock{};
        curLap.lapNum = lapNum;
        curLap.startSessionTime = lapStartTime;
        curLap.invalid = invalid;
        return;
    }
    if (lapNum > curLapNum) {
        finalizeCurrentLap(lastLapMs);           // lastLapMs is the just-finished lap's time
        curLapNum    = lapNum;
        lapStartTime = latestTime;
        curLap = LapBlock{};
        curLap.lapNum = lapNum;
        curLap.startSessionTime = lapStartTime;
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
    laps.clear();
    curLap = LapBlock{};
    curLapNum = -1;
    lapStartTime = 0;
    fastestLapNum = -1;
    fastestLapMs = INT_MAX;
    latestTime = 0;
}

void SessionData::trim() {
    if (!trimBuffers) return;
    const float cutoff = latestTime - BUFFER_S;
    if (cutoff <= 0) return;
    int dt = 0; while (dt < telBuf.size() && telBuf[dt].t < cutoff) ++dt;
    if (dt > 0) telBuf.remove(0, dt);
    int ds = 0; while (ds < stsBuf.size() && stsBuf[ds].t < cutoff) ++ds;
    if (ds > 0) stsBuf.remove(0, ds);
    int dm = 0; while (dm < motionBuf.size() && motionBuf[dm].t < cutoff) ++dm;
    if (dm > 0) motionBuf.remove(0, dm);
    int dmx = 0; while (dmx < motionExBuf.size() && motionExBuf[dmx].t < cutoff) ++dmx;
    if (dmx > 0) motionExBuf.remove(0, dmx);
}

const LapBlock* SessionData::lapByNum(int n) const {
    for (const LapBlock& l : laps) if (l.lapNum == n) return &l;
    return nullptr;
}

const LapBlock* SessionData::lapAtTime(float t) const {
    for (const LapBlock& l : laps)
        if (t >= l.startSessionTime && t <= l.endSessionTime) return &l;
    return nullptr;
}

// ── SessionModel: QObject wrapper + ~30 Hz coalescing ───────────────────────

SessionModel::SessionModel(QObject* parent) : QObject(parent) {
    flush_ = new QTimer(this);
    flush_->setInterval(33);   // ~30 Hz
    connect(flush_, &QTimer::timeout, this, [this] {
        if (telemetryDirty_) { telemetryDirty_ = false; emit telemetryAppended(); }
    });
    flush_->start();
}

void SessionModel::setLiveFlushActive(bool on) {
    if (!flush_) return;
    if (on) {
        if (!flush_->isActive()) {
            telemetryDirty_ = true;   // force one flush so charts catch up on the data ingested while paused
            flush_->start();
        }
    } else {
        flush_->stop();               // ingest keeps running; samples accumulate until resumed
    }
}

void SessionModel::onTelemetry(float t, float speed, int rpm, int gear, float throttle, float brake, float steering) {
    d_.onTelemetry(t, speed, rpm, gear, throttle, brake, steering);
    telemetryDirty_ = true;
}

void SessionModel::onStatus(float t, float ers, float fuel_kg, float ice_kw, float mguk_kw, float mguk_harvest_j, float mguh_harvest_j) {
    d_.onStatus(t, ers, fuel_kg, ice_kw, mguk_kw, mguk_harvest_j, mguh_harvest_j);
    telemetryDirty_ = true;
}

void SessionModel::onMotion(float t, float g_lat, float g_long) {
    d_.onMotion(t, g_lat, g_long);
    telemetryDirty_ = true;
}

void SessionModel::onMotionEx(float t, float front_aero, float rear_aero) {
    d_.onMotionEx(t, front_aero, rear_aero);
    telemetryDirty_ = true;
}

void SessionModel::onLap(int lapNum, int currentLapMs, int lastLapMs, bool invalid) {
    const int before = d_.laps.size();
    d_.onLap(lapNum, currentLapMs, lastLapMs, invalid);
    if (d_.laps.size() != before) emit lapsChanged();
}

void SessionModel::onSessionReset(float newTime) {
    d_.onSessionReset(newTime);
    emit wasReset();
    emit lapsChanged();
}

void SessionModel::truncateAfter(float newTime) {
    d_.truncateAfter(newTime);
    // Charts re-query the (now shorter) buffers; lap selectors re-read the laps.
    emit lapsChanged();
    emit telemetryAppended();
}

void SessionModel::clear() {
    d_.clear();
    emit wasReset();
    emit lapsChanged();
}

void SessionModel::load(SessionData&& data) {
    d_ = std::move(data);
    telemetryDirty_ = false;
    emit lapsChanged();
    emit telemetryAppended();
}
