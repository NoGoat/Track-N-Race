#include "TnrdPlayer.h"

#include <QDir>
#include <QStandardPaths>
#include <QDateTime>

#include <algorithm>
#include <cstring>

// Sparse types are those whose last-known value should be re-broadcast on seek
// so the UI can reconstruct state from any point in the file.
// IDs: 5=session, 7=timing, 8=participants, 9=all_status, 10=tyre_sets
static constexpr uint8_t SPARSE_TYPE_IDS[] = { 5, 7, 8, 9, 10 };

uint8_t TnrdPlayer::typeId(const std::string& s) {
    if (s == "telemetry")   return 1;
    if (s == "status")      return 2;
    if (s == "damage")      return 3;
    if (s == "lap")         return 4;
    if (s == "session")     return 5;
    if (s == "race_event")  return 6;
    if (s == "timing")      return 7;
    if (s == "participants") return 8;
    if (s == "all_status")  return 9;
    if (s == "tyre_sets")   return 10;
    return 0;
}

bool TnrdPlayer::isSparse(uint8_t id) {
    for (uint8_t sp : SPARSE_TYPE_IDS)
        if (id == sp) return true;
    return false;
}

TnrdPlayer::TnrdPlayer(QObject* parent)
    : QObject(parent)
{
    timer_ = new QTimer(this);
    timer_->setInterval(16); // ~60fps
    connect(timer_, &QTimer::timeout, this, &TnrdPlayer::tick);
}

TnrdPlayer::~TnrdPlayer() {
    cleanup();
}

// ── Public API ─────────────────────────────────────────────────────────────

bool TnrdPlayer::load(const QString& path) {
    cleanup();

    // Build temp file path
    QString tmpDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    tempPath_ = QDir(tmpDir).filePath(
        "tracknrace_" + QString::number(QDateTime::currentMSecsSinceEpoch()) + ".tmp");

    if (!decompress(path)) {
        tempPath_.clear();
        return false;
    }

    tempFile_.setFileName(tempPath_);
    if (!tempFile_.open(QIODevice::ReadOnly)) {
        QFile::remove(tempPath_);
        tempPath_.clear();
        return false;
    }

    // Read and emit header (first line)
    qint64 firstLineStart = tempFile_.pos();
    QByteArray headerLine = tempFile_.readLine();
    nlohmann::json header;
    try {
        header = nlohmann::json::parse(headerLine.constData(),
                                        headerLine.constData() + headerLine.size());
        if (!header.contains("magic") || header["magic"] != "TNRD_V1") {
            tempFile_.close();
            QFile::remove(tempPath_);
            tempPath_.clear();
            return false;
        }
    } catch (...) {
        tempFile_.close();
        QFile::remove(tempPath_);
        tempPath_.clear();
        return false;
    }

    Q_UNUSED(firstLineStart)
    buildIndex();
    emitState();
    emit loaded(header);
    return true;
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
    playing_ = false;
    timer_->stop();
    cleanup();
    emitState();
}

void TnrdPlayer::seek(float pct) {
    if (index_.empty()) return;

    float targetTime = std::max(0.0f, std::min(pct * totalTime_, totalTime_));

    bool wasPlaying = playing_;
    if (playing_) {
        playing_ = false;
        timer_->stop();
    }

    currentTime_ = targetTime;

    // Find playPos_: first index entry with sessionTime > targetTime
    size_t lo = 0, hi = index_.size();
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (index_[mid].sessionTime <= targetTime) lo = mid + 1;
        else hi = mid;
    }
    playPos_ = lo;

    // Re-broadcast last known value of each sparse type up to targetTime
    for (auto& [tid, positions] : sparseIdx_) {
        // Binary search: find last position where index_[pos].sessionTime <= targetTime
        auto it = std::upper_bound(positions.begin(), positions.end(), targetTime,
            [this](float t, size_t idxPos) {
                return t < index_[idxPos].sessionTime;
            });
        if (it != positions.begin()) {
            --it;
            nlohmann::json j = readLineAt(index_[*it].offset);
            if (!j.is_null()) emit packetReady(j);
        }
    }

    if (wasPlaying) {
        playing_ = true;
        elapsed_.start();
        timer_->start();
    }

    emitState();
}

void TnrdPlayer::setSpeed(float mult) {
    speed_ = mult;
    emitState();
}

// ── Timer tick ─────────────────────────────────────────────────────────────

void TnrdPlayer::tick() {
    qint64 deltaMs = elapsed_.restart();
    currentTime_ += (float)(deltaMs / 1000.0) * speed_;

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

bool TnrdPlayer::decompress(const QString& srcPath) {
    gzFile gz = nullptr;
#ifdef _WIN32
    gz = gzopen_w(srcPath.toStdWString().c_str(), "rb");
#else
    gz = gzopen(srcPath.toUtf8().constData(), "rb");
#endif
    if (!gz) return false;

    QFile out(tempPath_);
    if (!out.open(QIODevice::WriteOnly)) {
        gzclose(gz);
        return false;
    }

    char buf[131072];
    int n;
    while ((n = gzread(gz, buf, sizeof(buf))) > 0)
        out.write(buf, n);

    bool ok = (n >= 0);
    gzclose(gz);
    out.close();
    return ok;
}

void TnrdPlayer::buildIndex() {
    index_.clear();
    sparseIdx_.clear();
    totalTime_ = 0.0f;
    currentTime_ = 0.0f;
    playPos_ = 0;

    tempFile_.seek(0);
    // Skip header line
    tempFile_.readLine();

    while (!tempFile_.atEnd()) {
        qint64 lineStart = tempFile_.pos();
        QByteArray line = tempFile_.readLine();
        if (line.isEmpty() || line == "\n") continue;

        try {
            auto j = nlohmann::json::parse(line.constData(),
                                            line.constData() + line.size());
            float t = j.value("session_time", 0.0f);
            uint8_t tid = typeId(j.value("type", std::string{}));

            index_.push_back({ lineStart, t, tid });
            totalTime_ = std::max(totalTime_, t);

            if (isSparse(tid))
                sparseIdx_[tid].push_back(index_.size() - 1);

        } catch (...) {}
    }
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
    emit stateChanged(playing_, currentTime_, totalTime_, speed_);
}

void TnrdPlayer::cleanup() {
    if (tempFile_.isOpen()) tempFile_.close();
    if (!tempPath_.isEmpty()) {
        QFile::remove(tempPath_);
        tempPath_.clear();
    }
    index_.clear();
    sparseIdx_.clear();
    totalTime_   = 0.0f;
    currentTime_ = 0.0f;
    playPos_     = 0;
    playing_     = false;
}
