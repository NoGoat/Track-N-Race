#include "TnrdPlayer.h"

#include <QMetaObject>

#include <algorithm>
#include <cstring>
#include <thread>

// Fast type tag extraction from a raw JSON line — avoids a full parse for the
// cold rows the scan ignores. Mirrors the tags libtnrp writes.
static int scanType(const char* d, int len) {
    static const char KEY[] = "\"type\":\"";
    static constexpr int KLEN = sizeof(KEY) - 1;
    for (int i = 0; i <= len - KLEN; ++i) {
        if (d[i] == '"' && memcmp(d + i, KEY, KLEN) == 0) {
            const char* v = d + i + KLEN;
            int r = len - (i + KLEN);
            if (r >= 9  && memcmp(v, "telemetry",   9)  == 0) return 1;
            if (r >= 6  && memcmp(v, "status",      6)  == 0) return 2;
            if (r >= 6  && memcmp(v, "damage",      6)  == 0) return 3;
            if (r >= 4  && memcmp(v, "lap\"",       4)  == 0) return 4;
            if (r >= 7  && memcmp(v, "motion\"",    7)  == 0) return 11;
            if (r >= 10 && memcmp(v, "motion_ex\"", 10) == 0) return 12;
            return 0;
        }
    }
    return 0;
}

TnrdPlayer::TnrdPlayer(QObject* parent) : QObject(parent) {
    timer_ = new QTimer(this);
    timer_->setInterval(16);
    connect(timer_, &QTimer::timeout, this, &TnrdPlayer::tick);
}

TnrdPlayer::~TnrdPlayer() {
    cancelled_ = true;   // cleanup() joins the load thread (see below)
    cleanup();
}

// ── Public API ─────────────────────────────────────────────────────────────

void TnrdPlayer::load(const QString& path) {
    if (loading_) return;
    cleanup();               // reaps the previous (finished) load thread, closes the reader
    cancelled_ = false;
    loading_   = true;

    emit loadingStarted();

    const std::string src = path.toStdString();

    // The reader decompresses + indexes the whole file and we walk it once to
    // build the chart session — all on a background thread so the UI stays live.
    // Joined (not detached) in the destructor / next load, so the reader can't be
    // torn down while this thread is still using it.
    loadThread_ = std::thread([this, src] {
        auto fail = [this] {
            QMetaObject::invokeMethod(this, [this] {
                loading_ = false;
                emit loadFailed();
            }, Qt::QueuedConnection);
        };

        qInfo("[player] load requested: %s", src.c_str());
        if (cancelled_) { qInfo("[player] load cancelled before start"); fail(); return; }

        tnrp::HeaderRow header;
        if (!reader_.load(src, header) || cancelled_) {
            qWarning("[player] reader load failed%s for: %s (see [tnrd] logs above for the reason)",
                     cancelled_ ? " (cancelled)" : "", src.c_str());
            reader_.close();
            fail();
            return;
        }

        startTime_   = reader_.startTime();
        totalTime_   = reader_.totalTime();
        currentTime_ = startTime_;
        qInfo("[player] reader loaded: start=%.2f total=%.2f duration=%.2f",
              startTime_, totalTime_, totalTime_ - startTime_);

        scanIntoSessionData();
        if (cancelled_) { reader_.close(); fail(); return; }

        reader_.setCursor(startTime_);   // rewind the stream cursor for playback

        QMetaObject::invokeMethod(this, [this, header] {
            if (cancelled_) { reader_.close(); loading_ = false; return; }
            loading_ = false;
            emitState();
            emit loaded(header);

            // Emit the reconstructed frame at the start so panels — and the track
            // map, which needs the one-shot participants row to draw car dots at
            // all — show the initial state immediately, instead of only after the
            // first seek. Mirrors seek(): the cold state snapshot (which includes
            // participants) plus the latest status/damage/positions (which the
            // snapshot omits). Type IDs: 2=status, 3=damage, 13=positions.
            emitRows(reader_.stateSnapshot(startTime_));
            static const std::vector<uint8_t> kInitStateTypes = { 2, 3, 13 };
            emitRows(reader_.latestOfTypes(startTime_, kInitStateTypes));
        }, Qt::QueuedConnection);
    });
}

void TnrdPlayer::play() {
    if (playing_ || !reader_.isLoaded()) return;
    playing_ = true;
    elapsed_.start();
    timer_->start();
    emitState();
}

void TnrdPlayer::pause() {
    if (!playing_) return;
    playing_ = false;
    timer_->stop();
    emitState();
}

void TnrdPlayer::close() {
    cancelled_ = true;
    playing_   = false;
    timer_->stop();
    cleanup();
    emitState();
}

void TnrdPlayer::seek(float pct) {
    if (!reader_.isLoaded()) return;

    bool wasPlaying = playing_;
    if (playing_) { playing_ = false; timer_->stop(); }

    const float duration   = std::max(0.0f, totalTime_ - startTime_);
    const float targetTime = startTime_ + std::clamp(pct, 0.0f, 1.0f) * duration;
    currentTime_ = targetTime;

    reader_.setCursor(targetTime);

    // The UI drops transient history (the live chart) before we push the
    // reconstructed state for this point.
    emit seeked();

    // Cold state snapshot (lap/session/timing/participants/all_status/tyre_sets).
    // The reader's snapshot omits status/damage/positions, so replay the latest of
    // those too, so the race panel, damage rows and track map reflect the seek
    // target. The chart itself needs no window — the whole session lives in
    // SessionModel and keys off currentTime_.
    emitRows(reader_.stateSnapshot(targetTime));

    // Latest status / damage / positions at the seek point via a backward index
    // walk that reads only those rows. The previous approach read the whole 30 s
    // window (~5k string allocations per seek on a dense race), which dominated
    // scrub cost. Type IDs: 2=status, 3=damage, 13=positions.
    static const std::vector<uint8_t> kSeekStateTypes = { 2, 3, 13 };
    emitRows(reader_.latestOfTypes(targetTime, kSeekStateTypes));

    if (wasPlaying) { playing_ = true; elapsed_.start(); timer_->start(); }
    emitState();
}

void TnrdPlayer::seekToTime(float targetTime) {
    if (!reader_.isLoaded()) return;
    const float duration = std::max(0.0f, totalTime_ - startTime_);
    float pct = duration > 0.0f ? (targetTime - startTime_) / duration : 0.0f;
    seek(pct);
}

void TnrdPlayer::setSpeed(float mult) {
    speed_ = mult;
    emitState();
}

// ── Timer tick ─────────────────────────────────────────────────────────────

void TnrdPlayer::tick() {
    qint64 deltaMs = elapsed_.restart();
    // Cap how much session time a single tick advances, so a UI stall (or a high
    // speed multiplier) can't emit a huge batch synchronously and freeze the app.
    double step = (deltaMs / 1000.0) * speed_;
    if (step > 0.10) step = 0.10;
    currentTime_ += (float)step;

    if (currentTime_ >= totalTime_) {
        currentTime_ = totalTime_;
        emitRows(reader_.drainRest());
        playing_ = false;
        timer_->stop();
        emitState();
        emit finished();
        return;
    }

    emitRows(reader_.pullUntil(currentTime_));
    emitState();
}

// ── Private helpers ────────────────────────────────────────────────────────

void TnrdPlayer::scanIntoSessionData() {
    scanned_.clear();
    scanned_.trimBuffers = false;   // playback keeps the whole session in memory

    float lastDmg[4] = { 0.0f, 0.0f, 0.0f, 0.0f };  // last tyre wear seen, merged into tyre samples

    // Every row in the recording, in time order. The reader returns raw JSONL; the
    // scanType pre-filter skips the row types the charts ignore, and the survivors
    // are glaze-parsed into typed structs via tnrp::parseRow (fields the structs
    // don't declare are skipped — no dynamic JSON objects anywhere in the scan).
    std::vector<std::string> rows = reader_.readRange(startTime_, totalTime_);
    qInfo("[player] scan: %zu raw rows in [%.2f, %.2f]", rows.size(), startTime_, totalTime_);
    int parseFails = 0;
    for (const std::string& s : rows) {
        if (cancelled_) { qInfo("[player] scan cancelled"); return; }
        int tid = scanType(s.data(), (int)s.size());
        if (tid != 1 && tid != 2 && tid != 3 && tid != 4 && tid != 11 && tid != 12)
            continue;
        std::optional<tnrp::AnyRow> parsed = tnrp::parseRow(s);
        if (!parsed) { ++parseFails; continue; }

        if (const TelemetryRow* t = std::get_if<TelemetryRow>(&*parsed)) {
            scanned_.onTelemetry(t->session_time, (float)t->speed_kph, t->rpm, t->gear,
                                 t->throttle, t->brake, (float)t->steering);
            scanned_.onTyre(t->session_time,
                (float)t->tyre_temp_surface_fl, (float)t->tyre_temp_surface_fr,
                (float)t->tyre_temp_surface_rl, (float)t->tyre_temp_surface_rr,
                (float)t->tyre_temp_inner_fl,   (float)t->tyre_temp_inner_fr,
                (float)t->tyre_temp_inner_rl,   (float)t->tyre_temp_inner_rr,
                (float)t->brake_temp_fl,        (float)t->brake_temp_fr,
                (float)t->brake_temp_rl,        (float)t->brake_temp_rr,
                lastDmg[0], lastDmg[1], lastDmg[2], lastDmg[3]);
        } else if (const StatusRow* st = std::get_if<StatusRow>(&*parsed)) {
            scanned_.onStatus(st->session_time,
                              (float)st->ers_pct,
                              (float)st->fuel_kg,
                              (float)st->engine_power_ice_kw,
                              (float)st->engine_power_mguk_kw,
                              (float)st->ers_harvested_mguk_j,
                              (float)st->ers_harvested_mguh_j,
                              st->tyre_compound, st->visual_compound);
        } else if (const DamageRow* d = std::get_if<DamageRow>(&*parsed)) {
            lastDmg[0] = (float)d->tyre_wear_fl;
            lastDmg[1] = (float)d->tyre_wear_fr;
            lastDmg[2] = (float)d->tyre_wear_rl;
            lastDmg[3] = (float)d->tyre_wear_rr;
            scanned_.onDamage(d->session_time, lastDmg[0], lastDmg[1], lastDmg[2], lastDmg[3]);
        } else if (const LapRow* l = std::get_if<LapRow>(&*parsed)) {
            scanned_.onLap(l->lap_num, l->current_lap_ms, l->last_lap_ms, l->lap_invalid);
        } else if (const MotionRow* m = std::get_if<MotionRow>(&*parsed)) {
            scanned_.onMotion(m->session_time, (float)m->g_lat, (float)m->g_long);
        } else if (const MotionExRow* me = std::get_if<MotionExRow>(&*parsed)) {
            scanned_.onMotionEx(me->session_time,
                                (float)me->front_aero_height_mm,
                                (float)me->rear_aero_height_mm);
        }
    }

    // Close the trailing in-progress lap and sort everything by time, since UDP
    // packets can be recorded slightly out of order.
    scanned_.finalizeOpenLap();
    auto byT = [](const auto& a, const auto& b) { return a.t < b.t; };
    std::sort(scanned_.telBuf.begin(),      scanned_.telBuf.end(),      byT);
    std::sort(scanned_.stsBuf.begin(),      scanned_.stsBuf.end(),      byT);
    std::sort(scanned_.motionBuf.begin(),   scanned_.motionBuf.end(),   byT);
    std::sort(scanned_.motionExBuf.begin(), scanned_.motionExBuf.end(), byT);
    std::sort(scanned_.tyreBuf.begin(),     scanned_.tyreBuf.end(),     byT);
    std::sort(scanned_.damageBuf.begin(),   scanned_.damageBuf.end(),   byT);
    for (LapBlock& l : scanned_.laps) {
        std::sort(l.tel.begin(), l.tel.end(), byT);
        std::sort(l.sts.begin(), l.sts.end(), byT);
    }

    qInfo("[player] scan done: tel=%d sts=%d tyre=%d motion=%d motionEx=%d laps=%d parseFails=%d",
          (int)scanned_.telBuf.size(),  (int)scanned_.stsBuf.size(),
          (int)scanned_.tyreBuf.size(), (int)scanned_.motionBuf.size(),
          (int)scanned_.motionExBuf.size(), (int)scanned_.laps.size(), parseFails);
}

void TnrdPlayer::emitRows(const std::vector<std::string>& rows) {
    for (const std::string& s : rows) {
        std::optional<tnrp::AnyRow> parsed = tnrp::parseRow(s);
        if (parsed) emit packetReady(*parsed);
    }
}

void TnrdPlayer::emitState() {
    // Report times relative to the session start so the slider and label run from
    // 0:00 to the recording's duration regardless of the absolute clock.
    emit stateChanged(playing_, currentTime_ - startTime_, totalTime_ - startTime_, speed_);
}

void TnrdPlayer::cleanup() {
    // Join any background load first so we never close the reader (deleting its
    // temp file) while that thread is still using it. On cancellation the thread
    // closes the reader itself before returning, so the temp is never leaked.
    // Safe from deadlock: the load thread only ever posts to the UI thread via
    // QueuedConnection (non-blocking) and never calls cleanup() itself.
    if (loadThread_.joinable()) loadThread_.join();
    reader_.close();
    startTime_   = 0.0f;
    totalTime_   = 0.0f;
    currentTime_ = 0.0f;
    playing_     = false;
}
