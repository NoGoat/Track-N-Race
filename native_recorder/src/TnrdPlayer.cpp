#include "TnrdPlayer.h"

#include <QDir>
#include <QStandardPaths>
#include <QDateTime>
#include <QMetaObject>

#include <algorithm>
#include <thread>
#include <cstring>
#include <cstdlib>
#include <unordered_set>

// State-defining packet types reconstructed on seek (everything the UI tracks
// except telemetry, which is refilled via the dense window, and race_event,
// which is an append-only log that must not be replayed). See emitLiveData().
static constexpr uint8_t RECON_TYPE_IDS[] = { 1, 2, 3, 4, 5, 7, 8, 9, 10 };

// Fast field extraction from a raw JSON line — avoids full JSON parsing during indexing.

// Returns -1.0f when the line has no session_time field, so the caller can carry
// the previous timestamp forward (some packets, e.g. participants, omit it).
static float scanSessionTime(const char* d, int len) {
    static const char KEY[] = "\"session_time\":";
    static constexpr int KLEN = sizeof(KEY) - 1;
    for (int i = 0; i <= len - KLEN; ++i) {
        if (d[i] == '"' && memcmp(d + i, KEY, KLEN) == 0)
            return strtof(d + i + KLEN, nullptr);
    }
    return -1.0f;
}

static uint8_t scanType(const char* d, int len) {
    static const char KEY[] = "\"type\":\"";
    static constexpr int KLEN = sizeof(KEY) - 1;
    for (int i = 0; i <= len - KLEN; ++i) {
        if (d[i] == '"' && memcmp(d + i, KEY, KLEN) == 0) {
            const char* v = d + i + KLEN;
            int r = len - (i + KLEN);
            // Ordered by expected frequency
            if (r >= 9  && memcmp(v, "telemetry",   9)  == 0) return 1;
            if (r >= 6  && memcmp(v, "timing",       6)  == 0) return 7;
            if (r >= 3  && memcmp(v, "lap\"",        4)  == 0) return 4;
            if (r >= 6  && memcmp(v, "status",       6)  == 0) return 2;
            if (r >= 10 && memcmp(v, "all_status",  10)  == 0) return 9;
            if (r >= 12 && memcmp(v, "participants", 12) == 0) return 8;
            if (r >= 6  && memcmp(v, "damage",       6)  == 0) return 3;
            if (r >= 8  && memcmp(v, "session\"",     8)  == 0) return 5;
            if (r >= 10 && memcmp(v, "race_event",  10)  == 0) return 6;
            if (r >= 9  && memcmp(v, "tyre_sets",    9)  == 0) return 10;
            return 0;
        }
    }
    return 0;
}

uint8_t TnrdPlayer::typeId(const std::string& s) {
    if (s == "telemetry")    return 1;
    if (s == "status")       return 2;
    if (s == "damage")       return 3;
    if (s == "lap")          return 4;
    if (s == "session")      return 5;
    if (s == "race_event")   return 6;
    if (s == "timing")       return 7;
    if (s == "participants") return 8;
    if (s == "all_status")   return 9;
    if (s == "tyre_sets")    return 10;
    return 0;
}

TnrdPlayer::TnrdPlayer(QObject* parent) : QObject(parent) {
    timer_ = new QTimer(this);
    timer_->setInterval(16);
    connect(timer_, &QTimer::timeout, this, &TnrdPlayer::tick);
}

TnrdPlayer::~TnrdPlayer() {
    cancelled_ = true;
    cleanup();
}

// ── Public API ─────────────────────────────────────────────────────────────

void TnrdPlayer::load(const QString& path) {
    if (loading_) return;
    cleanup();
    cancelled_ = false;
    loading_   = true;

    QString tmpDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    tempPath_ = QDir(tmpDir).filePath(
        "tracknrace_" + QString::number(QDateTime::currentMSecsSinceEpoch()) + ".tmp");

    emit loadingStarted();

    // Capture by value so the background thread owns its own copies
    QString srcPath  = path;
    QString destPath = tempPath_;

    std::thread([this, srcPath, destPath] {
        auto fail = [this, destPath] {
            QFile::remove(destPath);
            QMetaObject::invokeMethod(this, [this] {
                loading_ = false;
                emit loadFailed();
            }, Qt::QueuedConnection);
        };

        if (cancelled_) { fail(); return; }

        if (!decompress(srcPath, destPath)) { fail(); return; }
        if (cancelled_) { fail(); return; }

        // Read header line into a local handle (safe from background thread)
        nlohmann::json header;
        {
            QFile f(destPath);
            if (!f.open(QIODevice::ReadOnly)) { fail(); return; }
            QByteArray line = f.readLine();
            try {
                header = nlohmann::json::parse(line.constData(),
                                                line.constData() + line.size());
                if (!header.contains("magic") || header["magic"] != "TNRD_V1") {
                    fail(); return;
                }
            } catch (...) { fail(); return; }
        }

        if (cancelled_) { fail(); return; }

        buildIndex(destPath);

        if (cancelled_) { fail(); return; }

        QMetaObject::invokeMethod(this, [this, header, destPath] {
            if (cancelled_) {
                QFile::remove(destPath);
                loading_ = false;
                return;
            }
            // Open the member file handle on the main thread for playback use
            tempFile_.setFileName(destPath);
            if (!tempFile_.open(QIODevice::ReadOnly)) {
                loading_ = false;
                emit loadFailed();
                return;
            }
            loading_ = false;
            emitState();
            emit loaded(header);
        }, Qt::QueuedConnection);
    }).detach();
}

void TnrdPlayer::play() {
    if (playing_ || index_.empty()) return;
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

// First index whose sessionTime is strictly greater than t (assumes index_ sorted by time).
size_t TnrdPlayer::upperBoundTime(float t) const {
    size_t lo = 0, hi = index_.size();
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (index_[mid].sessionTime <= t) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

// First index whose sessionTime is >= t.
size_t TnrdPlayer::lowerBoundTime(float t) const {
    size_t lo = 0, hi = index_.size();
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (index_[mid].sessionTime < t) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

void TnrdPlayer::seek(float pct) {
    if (index_.empty()) return;

    bool wasPlaying = playing_;
    if (playing_) { playing_ = false; timer_->stop(); }

    const float duration   = std::max(0.0f, totalTime_ - startTime_);
    const float targetTime = startTime_ + std::clamp(pct, 0.0f, 1.0f) * duration;
    currentTime_ = targetTime;

    // Everything at or before targetTime is considered already played.
    playPos_ = upperBoundTime(targetTime);

    // The UI drops transient history (the live chart) before we push the
    // reconstructed state for this point.
    emit seeked();

    // ── Phase 1: panel-state snapshot (cheap, synchronous) ────────────────
    // Walk the index backward once, collecting the most recent occurrence of
    // each state type at or before the target — a handful of line reads. The
    // dense history is NOT replayed here; that would rebuild every panel
    // thousands of times. Analogous to Electron's broadcastInitialState().
    std::unordered_set<uint8_t> wanted(std::begin(RECON_TYPE_IDS), std::end(RECON_TYPE_IDS));
    std::vector<size_t> snapshot;
    for (size_t i = playPos_; i-- > 0 && !wanted.empty(); ) {
        if (wanted.erase(index_[i].type))
            snapshot.push_back(i);
    }
    std::sort(snapshot.begin(), snapshot.end());  // chronological order
    for (size_t idx : snapshot) {
        nlohmann::json j = readLineAt(index_[idx].offset);
        if (!j.is_null()) emit packetReady(j);
    }

    // The chart no longer reloads a window on seek: the whole session lives in
    // SessionModel, so moving currentTime_ is enough — the chart re-queries it.
    if (wasPlaying) { playing_ = true; elapsed_.start(); timer_->start(); }
    emitState();
}

void TnrdPlayer::setSpeed(float mult) {
    speed_ = mult;
    emitState();
}

// ── Timer tick ─────────────────────────────────────────────────────────────

void TnrdPlayer::tick() {
    qint64 deltaMs = elapsed_.restart();
    // Cap how much session time a single tick advances. If the UI thread stalls
    // (or speed is high), this stops one tick from emitting a huge packet batch
    // synchronously and freezing the app — playback degrades gracefully instead.
    double step = (deltaMs / 1000.0) * speed_;
    if (step > 0.10) step = 0.10;
    currentTime_ += (float)step;

    if (currentTime_ >= totalTime_) {
        currentTime_ = totalTime_;
        while (playPos_ < index_.size()) {
            nlohmann::json j = readLineAt(index_[playPos_].offset);
            if (!j.is_null()) emit packetReady(j);
            ++playPos_;
        }
        playing_ = false;
        timer_->stop();
        emitState();
        emit finished();
        return;
    }

    while (playPos_ < index_.size() && index_[playPos_].sessionTime <= currentTime_) {
        nlohmann::json j = readLineAt(index_[playPos_].offset);
        if (!j.is_null()) emit packetReady(j);
        ++playPos_;
    }

    emitState();
}

// ── Private helpers ────────────────────────────────────────────────────────

bool TnrdPlayer::decompress(const QString& srcPath, const QString& destPath) {
    gzFile gz = nullptr;
#ifdef _WIN32
    gz = gzopen_w(srcPath.toStdWString().c_str(), "rb");
#else
    gz = gzopen(srcPath.toUtf8().constData(), "rb");
#endif
    if (!gz) return false;

    QFile out(destPath);
    if (!out.open(QIODevice::WriteOnly)) { gzclose(gz); return false; }

    char buf[131072];
    int n;
    while ((n = gzread(gz, buf, sizeof(buf))) > 0)
        out.write(buf, n);

    bool ok = (n >= 0);
    gzclose(gz);
    out.close();
    return ok;
}

void TnrdPlayer::buildIndex(const QString& filePath) {
    index_.clear();
    scanned_.clear();
    scanned_.trimBuffers = false;   // playback keeps the whole session in memory
    startTime_   = 0.0f;
    totalTime_   = 0.0f;
    currentTime_ = 0.0f;
    playPos_     = 0;

    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly)) return;
    f.readLine(); // skip header

    constexpr qint64 CHUNK = 2 * 1024 * 1024; // 2 MB per read
    QByteArray buf(CHUNK, Qt::Uninitialized);

    QByteArray partial;              // incomplete line carried over from the previous chunk
    qint64 partialOffset = f.pos(); // file offset where `partial` begins

    float lastT = 0.0f;   // carried forward to packets that omit session_time
    bool  haveStart = false;

    auto commitLine = [&](const char* ld, int ll, qint64 lineOffset) {
        if (ll <= 1) return;
        float t = scanSessionTime(ld, ll);
        if (t < 0.0f) t = lastT;   // inherit previous timestamp when field is absent
        else          lastT = t;
        if (!haveStart) { startTime_ = t; haveStart = true; }
        uint8_t tid = scanType(ld, ll);
        index_.push_back({ lineOffset, t, tid });
        totalTime_ = std::max(totalTime_, t);

        // Build the chart's session model in the same pass. Only the three row
        // types the chart needs are parsed; timing/participants/etc are skipped.
        if (tid == 1 || tid == 2 || tid == 4) {
            try {
                nlohmann::json j = nlohmann::json::parse(ld, ld + ll);
                if (tid == 1)
                    scanned_.onTelemetry(t,
                                         j.value("speed_kph", 0.0f),
                                         j.value("rpm", 0),
                                         j.value("gear", 0),
                                         j.value("throttle", 0.0f),
                                         j.value("brake", 0.0f),
                                         j.value("steering", 0.0f));
                else if (tid == 2)
                    scanned_.onStatus(t, j.value("ers_pct", 0.0f));
                else
                    scanned_.onLap(j.value("lap_num", 0),
                                   j.value("current_lap_ms", 0),
                                   j.value("last_lap_ms", 0),
                                   j.value("lap_invalid", false));
            } catch (...) {}
        }
    };

    while (!cancelled_) {
        qint64 chunkStart = f.pos();
        qint64 n = f.read(buf.data(), CHUNK);
        if (n <= 0) break;

        const char* base = buf.constData();
        const char* p    = base;
        const char* end  = base + n;

        while (p < end) {
            const char* nl = static_cast<const char*>(memchr(p, '\n', end - p));

            if (!nl) {
                // Tail of chunk is an incomplete line — carry it to the next iteration
                partial       = partial + QByteArray(p, (int)(end - p));
                // partialOffset already set correctly for the first piece;
                // if partial was empty, record where this segment starts in the file
                if (partial.size() == (int)(end - p))
                    partialOffset = chunkStart + (p - base);
                break;
            }

            if (!partial.isEmpty()) {
                // Complete the carried-over line
                QByteArray full = partial + QByteArray(p, (int)(nl - p));
                commitLine(full.constData(), full.size(), partialOffset);
                partial.clear();
            } else {
                qint64 lineOffset = chunkStart + (p - base);
                commitLine(p, (int)(nl - p), lineOffset);
            }

            p = nl + 1;
        }
    }

    // Flush any line that ends exactly at EOF (no trailing newline)
    if (!partial.isEmpty() && !cancelled_)
        commitLine(partial.constData(), partial.size(), partialOffset);

    // Close out the trailing (still-open) lap and sort everything by time, since
    // UDP packets can be written slightly out of order. Mirrors sessionPlayer.ts.
    scanned_.finalizeOpenLap();
    auto byT = [](auto a, auto b) { return a.t < b.t; };
    std::sort(scanned_.telBuf.begin(), scanned_.telBuf.end(), byT);
    std::sort(scanned_.stsBuf.begin(), scanned_.stsBuf.end(), byT);
    for (LapBlock& l : scanned_.laps) {
        std::sort(l.tel.begin(), l.tel.end(), byT);
        std::sort(l.sts.begin(), l.sts.end(), byT);
    }

    currentTime_ = startTime_;   // playback begins at the first packet's clock
}

nlohmann::json TnrdPlayer::readLineAt(qint64 offset) {
    if (!tempFile_.isOpen()) return {};
    tempFile_.seek(offset);
    QByteArray line = tempFile_.readLine();
    if (line.isEmpty()) return {};
    try {
        return nlohmann::json::parse(line.constData(), line.constData() + line.size());
    } catch (...) {
        return {};
    }
}

void TnrdPlayer::emitState() {
    // Report times relative to the session start so the slider and label run
    // from 0:00 to the recording's duration regardless of the absolute clock.
    emit stateChanged(playing_, currentTime_ - startTime_, totalTime_ - startTime_, speed_);
}

void TnrdPlayer::cleanup() {
    if (tempFile_.isOpen()) tempFile_.close();
    if (!tempPath_.isEmpty()) {
        QFile::remove(tempPath_);
        tempPath_.clear();
    }
    index_.clear();
    startTime_   = 0.0f;
    totalTime_   = 0.0f;
    currentTime_ = 0.0f;
    playPos_     = 0;
    playing_     = false;
}
