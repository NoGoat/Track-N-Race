#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QVector>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

#include <tnrp/control_rows.h>

#include "EngineSink.h"
#include "SessionModel.h"

namespace tnrp { class Engine; }

// A decoded native seek/history payload. It is assembled away from the GUI
// thread and moved into SessionModel with one short commit.
struct PlaybackHistoryBatch {
    SessionData data;
    QVector<LapProgressSample> progress;
    QVector<LapBlock> lapDetails;
    float currentLapStart = 0.0f;
    int lapNum = 0;
    bool additive = false;
    // True only for playback_lap_data. Seek/window history also carries a
    // positive current lap number, so lapNum alone cannot identify this case.
    bool isolatedLapRequest = false;
    bool authoritativeSeek = true;
    uint64_t requestId = 0;
    uint32_t rowTypeMask = 0;
    float historyStart = 0.0f;
};

struct PlaybackLapRange {
    int lapNum = 0;
    float start = 0.0f;
    float end = 0.0f;
};

// Qt command/state facade over the host's single tnrp::Engine. Blocking load,
// seek and history operations run on one bounded worker queue; play/pause/speed
// remain cheap engine state changes. Engine control rows are decoded here rather
// than being forced through AnyRow.
class TnrdPlayer : public QObject {
    Q_OBJECT
public:
    explicit TnrdPlayer(tnrp::Engine* engine, QObject* parent = nullptr);
    ~TnrdPlayer() override;

    void setEngine(tnrp::Engine* engine);
    void quiesce();
    void shutdown();

    void load(const QString& path);
    void play();
    void pause();
    void close();
    void seek(float pct);
    void seekToTime(float absoluteTime);
    void setSpeed(float mult);
    void setDataRequirements(uint32_t streamMask, uint32_t historyMask,
                             float windowSeconds);
    void requestLapData(int lapNum, uint32_t rowTypeMask);

    bool handleControlRow(const QByteArray& json);
    void handleSeekFlush(const std::shared_ptr<EngineSeekFlush>& flush);
    void completeSeek(uint64_t requestId);

    float totalTime() const { return totalTime_; }
    float currentTime() const { return currentTime_; }
    bool isPlaying() const { return playing_; }
    bool isLoaded() const { return loaded_.load(std::memory_order_acquire); }

signals:
    void loadingStarted();
    void loaded(const tnrp::HeaderRow& header);
    void lapBlocksReady(const tnrp::PlaybackLapBlocksRow& blocks);
    void loadFailed(const QString& reason);
    void stateChanged(bool playing, float currentTime, float totalTime, float speed);
    void seekStarted(uint64_t requestId);
    void historyDecoded(const std::shared_ptr<PlaybackHistoryBatch>& batch);
    void seeked();
    void finished();
    void closed();

private:
    enum class WorkKind {
        Load, Seek, Requirements, LapData,
        SeekDecode, RequirementsDecode, LapDataDecode, Close
    };
    struct WorkItem { WorkKind kind; std::function<void()> run; };

    void post(WorkKind kind, std::function<void()> work, bool replacePending);
    void workerLoop();
    static std::shared_ptr<PlaybackHistoryBatch>
        decodeHistory(const std::shared_ptr<EngineSeekFlush>& flush,
                      const QVector<PlaybackLapRange>& lapRanges);
    static std::shared_ptr<PlaybackHistoryBatch> decodeLapData(const QByteArray& json);

    std::atomic<tnrp::Engine*> engine_{nullptr};
    std::mutex workMutex_;
    std::condition_variable workCv_;
    std::condition_variable idleCv_;
    std::deque<WorkItem> work_;
    std::thread worker_;
    bool stopping_ = false;
    bool workerActive_ = false;

    float startTime_ = 0.0f;
    float totalTime_ = 0.0f;
    float currentTime_ = 0.0f;
    float speed_ = 1.0f;
    bool playing_ = false;
    std::atomic<bool> loaded_{false};
    std::atomic<bool> catalogReady_{false};
    bool loading_ = false;
    bool resumeAfterSeek_ = false;
    uint32_t streamMask_ = 0xFFFFFFFFu;
    uint32_t historyMask_ = 0;
    float historyWindowSeconds_ = 30.0f;
    bool requirementsApplied_ = false;
    QVector<PlaybackLapRange> lapRanges_;
    std::atomic<uint64_t> latestSeekRequest_{0};
    std::atomic<uint64_t> latestRequirementsRequest_{0};
};
