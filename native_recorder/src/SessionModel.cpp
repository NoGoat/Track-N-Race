#include "SessionModel.h"

#include <algorithm>

// ── SessionData: lap segmentation + buffering ───────────────────────────────

void SessionData::onTelemetry(float t, float speed, int rpm, int gear, float throttle, float brake, float steering) {
    latestTime = t;
    telBuf.push_back({ t, speed, rpm, gear, throttle, brake, steering });
    if (curLapNum >= 0) curLap.tel.push_back({ t, speed, rpm, gear, throttle, brake, steering });
    trim();
}

void SessionData::onStatus(float t, float ers) {
    stsBuf.push_back({ t, ers });
    if (curLapNum >= 0) curLap.sts.push_back({ t, ers });
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

void SessionData::clear() {
    telBuf.clear();
    stsBuf.clear();
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

void SessionModel::onTelemetry(float t, float speed, int rpm, int gear, float throttle, float brake, float steering) {
    d_.onTelemetry(t, speed, rpm, gear, throttle, brake, steering);
    telemetryDirty_ = true;
}

void SessionModel::onStatus(float t, float ers) {
    d_.onStatus(t, ers);
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
