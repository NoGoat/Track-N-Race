#pragma once

#include <QByteArray>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QObject>

#include <cstddef>
#include <cstdint>
#include <deque>
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
//
// Seek flushes share that ordered queue. A playback seek's history flush marks
// the boundary between rows from the old cursor and rows from the new one, and
// MainWindow's seek gate (Electron's waiting-flush / waiting-renderer phases)
// depends on seeing them in engine order. Coalescing rows emitted after a
// flush into a batch delivered before it would mix the two timelines.
class EngineSink : public QObject, public tnrp::Sink {
    Q_OBJECT
public:
    explicit EngineSink(QObject* parent = nullptr) : QObject(parent) {}

    void onRow(const std::string& json) override {
        bool schedule = false;
        {
            QMutexLocker lock(&mutex_);
            Event& event = dataEventLocked();
            if (!event.rows.isEmpty()) event.rows.append('\n');
            event.rows.append(json.data(), static_cast<qsizetype>(json.size()));
            schedule = scheduleLocked();
        }
        if (schedule)
            QMetaObject::invokeMethod(this, [this] { flushPending(); }, Qt::QueuedConnection);
    }

    void onBinary(const uint8_t* data, size_t len) override {
        if (!data || len == 0) return;
        bool schedule = false;
        {
            QMutexLocker lock(&mutex_);
            dataEventLocked().binary.append(reinterpret_cast<const char*>(data),
                                            static_cast<qsizetype>(len));
            schedule = scheduleLocked();
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
        bool schedule = false;
        {
            QMutexLocker lock(&mutex_);
            Event event;
            event.flush = std::move(flush);
            events_.push_back(std::move(event));
            schedule = scheduleLocked();
        }
        if (schedule)
            QMetaObject::invokeMethod(this, [this] { flushPending(); }, Qt::QueuedConnection);
    }

    void onPairState(const std::string& publicStateJson,
                     const std::string& persistedStateJson) override {
        const QByteArray publicState(publicStateJson.data(),
                                     static_cast<qsizetype>(publicStateJson.size()));
        const QByteArray persistedState(persistedStateJson.data(),
                                        static_cast<qsizetype>(persistedStateJson.size()));
        QMetaObject::invokeMethod(this, [this, publicState, persistedState] {
            emit pairStateReady(publicState, persistedState);
        }, Qt::QueuedConnection);
    }

    void onPairDiagnostic(const std::string& message) override {
        const QString text = QString::fromUtf8(message.data(),
                                               static_cast<qsizetype>(message.size()));
        QMetaObject::invokeMethod(this, [this, text] {
            emit pairDiagnosticReady(text);
        }, Qt::QueuedConnection);
    }

signals:
    void rowsReady(const QByteArray& jsonLines);
    void binaryReady(const QByteArray& batch);
    void seekFlushReady(const std::shared_ptr<EngineSeekFlush>& flush);
    void pairStateReady(const QByteArray& publicStateJson,
                        const QByteArray& persistedStateJson);
    void pairDiagnosticReady(const QString& message);

private:
    // One ordered unit of GUI delivery: either coalesced rows/binary or a
    // single seek flush.
    struct Event {
        QByteArray rows;
        QByteArray binary;
        std::shared_ptr<EngineSeekFlush> flush;
    };

    // The data event rows are appended to: the queue tail, unless that is a
    // seek flush (rows after a flush must be delivered after it).
    Event& dataEventLocked() {
        if (events_.empty() || events_.back().flush) events_.emplace_back();
        return events_.back();
    }

    bool scheduleLocked() {
        if (flushScheduled_) return false;
        flushScheduled_ = true;
        return true;
    }

    void flushPending() {
        std::deque<Event> events;
        {
            QMutexLocker lock(&mutex_);
            events.swap(events_);
            flushScheduled_ = false;
        }
        for (Event& event : events) {
            if (event.flush) {
                emit seekFlushReady(event.flush);
                continue;
            }
            if (!event.rows.isEmpty()) emit rowsReady(event.rows);
            if (!event.binary.isEmpty()) emit binaryReady(event.binary);
        }
    }

    QMutex mutex_;
    std::deque<Event> events_;
    bool flushScheduled_ = false;
};
