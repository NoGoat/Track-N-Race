#pragma once

#include <QByteArray>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QObject>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <tnrp/Sink.h>

// Shared ownership keeps a native seek store alive until the Qt playback worker
// has decoded it. Unlike Electron, the in-process host does not copy the packed
// bytes merely to cross an IPC boundary.
struct EngineSeekFlush {
    std::shared_ptr<const std::vector<uint8_t>> binaryStore;
    size_t binaryBegin = 0;
    size_t binaryEnd = 0;
    QByteArray coldJson;
    float currentLapStart = 0.0f;
    int lapNum = 0;
    bool allHistory = false;
    uint64_t requestId = 0;
    bool authoritativeSeek = true;
    uint32_t rowTypeMask = 0xFFFFFFFFu;
    float historyStart = 0.0f;
};

// Thread-safe, back-pressure-aware bridge from libtnrp to Qt. The engine may
// call each Sink method from UDP, playback, or command workers. Normal traffic
// is accumulated behind one queued GUI callback; if the GUI falls behind, the
// buffers grow into a larger batch instead of creating an event per row.
class EngineSink : public QObject, public tnrp::Sink {
    Q_OBJECT
public:
    explicit EngineSink(QObject* parent = nullptr) : QObject(parent) {}

    void onRow(const std::string& json) override {
        bool schedule = false;
        {
            QMutexLocker lock(&mutex_);
            if (!pendingRows_.isEmpty()) pendingRows_.append('\n');
            pendingRows_.append(json.data(), static_cast<qsizetype>(json.size()));
            if (!flushScheduled_) {
                flushScheduled_ = true;
                schedule = true;
            }
        }
        if (schedule)
            QMetaObject::invokeMethod(this, [this] { flushPending(); }, Qt::QueuedConnection);
    }

    void onBinary(const uint8_t* data, size_t len) override {
        if (!data || len == 0) return;
        bool schedule = false;
        {
            QMutexLocker lock(&mutex_);
            pendingBinary_.append(reinterpret_cast<const char*>(data),
                                  static_cast<qsizetype>(len));
            if (!flushScheduled_) {
                flushScheduled_ = true;
                schedule = true;
            }
        }
        if (schedule)
            QMetaObject::invokeMethod(this, [this] { flushPending(); }, Qt::QueuedConnection);
    }

    void onSeekFlush(std::shared_ptr<const std::vector<uint8_t>> binStore,
                     size_t binBegin, size_t binEnd, std::string&& coldJson,
                     float currentLapStart, int lapNum, bool allHistory,
                     uint64_t requestId, bool authoritativeSeek,
                     uint32_t rowTypeMask, float historyStart) override {
        auto flush = std::make_shared<EngineSeekFlush>();
        flush->binaryStore = std::move(binStore);
        flush->binaryBegin = binBegin;
        flush->binaryEnd = binEnd;
        flush->coldJson = QByteArray(coldJson.data(), static_cast<qsizetype>(coldJson.size()));
        flush->currentLapStart = currentLapStart;
        flush->lapNum = lapNum;
        flush->allHistory = allHistory;
        flush->requestId = requestId;
        flush->authoritativeSeek = authoritativeSeek;
        flush->rowTypeMask = rowTypeMask;
        flush->historyStart = historyStart;
        QMetaObject::invokeMethod(this, [this, flush = std::move(flush)] {
            emit seekFlushReady(flush);
        }, Qt::QueuedConnection);
    }

signals:
    void rowsReady(const QByteArray& jsonLines);
    void binaryReady(const QByteArray& batch);
    void seekFlushReady(const std::shared_ptr<EngineSeekFlush>& flush);

private:
    void flushPending() {
        QByteArray rows;
        QByteArray binary;
        {
            QMutexLocker lock(&mutex_);
            rows.swap(pendingRows_);
            binary.swap(pendingBinary_);
            flushScheduled_ = false;
        }
        if (!rows.isEmpty()) emit rowsReady(rows);
        if (!binary.isEmpty()) emit binaryReady(binary);
    }

    QMutex mutex_;
    QByteArray pendingRows_;
    QByteArray pendingBinary_;
    bool flushScheduled_ = false;
};
