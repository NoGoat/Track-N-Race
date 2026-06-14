#pragma once

#include <QObject>
#include <QTimer>
#include <QFile>
#include <QElapsedTimer>
#include <QString>
#include <QVector>
#include <atomic>
#include <vector>
#include <nlohmann/json.hpp>
#include <zlib.h>

// One pre-extracted point for the live chart's seek-history refill.
struct ChartSample {
    float t;
    float speed;
    float rpm;
    float ers;
};

class TnrdPlayer : public QObject {
    Q_OBJECT

public:
    explicit TnrdPlayer(QObject* parent = nullptr);
    ~TnrdPlayer() override;

    void load(const QString& path);  // non-blocking; result via signals
    void play();
    void pause();
    void close();
    void seek(float pct);
    void setSpeed(float mult);

    float totalTime()   const { return totalTime_; }
    float currentTime() const { return currentTime_; }
    bool  isPlaying()   const { return playing_; }

signals:
    void loadingStarted();
    void loaded(nlohmann::json header);
    void loadFailed();
    void packetReady(nlohmann::json packet);
    void stateChanged(bool playing, float currentTime, float totalTime, float speed);
    void seeked();
    void chartHistory(const QVector<ChartSample>& samples);
    void finished();

private slots:
    void tick();
    void flushChartWindow();

private:
    struct IndexEntry {
        qint64  offset;
        float   sessionTime;
        uint8_t type;
    };

    std::vector<IndexEntry> index_;

    float        startTime_   = 0.0f;
    float        totalTime_   = 0.0f;
    float        currentTime_ = 0.0f;
    float        speed_       = 1.0f;
    bool         playing_     = false;
    bool         loading_     = false;
    size_t       playPos_     = 0;

    std::atomic<bool> cancelled_{false};

    QString       tempPath_;
    QFile         tempFile_;
    QTimer*       timer_       = nullptr;
    QTimer*       flushTimer_  = nullptr;   // coalesces chart-history loads during slider drags
    float         pendingTarget_ = 0.0f;
    QElapsedTimer elapsed_;

    static uint8_t typeId(const std::string& s);

    size_t upperBoundTime(float t) const;  // first index with sessionTime > t
    size_t lowerBoundTime(float t) const;  // first index with sessionTime >= t

    // Both called from the background thread; use local file handles, not tempFile_
    bool decompress(const QString& srcPath, const QString& destPath);
    void buildIndex(const QString& filePath);

    nlohmann::json readLineAt(qint64 offset);
    void           emitState();
    void           cleanup();
};
