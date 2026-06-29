#pragma once

#include <QObject>
#include <QTimer>
#include <QElapsedTimer>
#include <QString>
#include <atomic>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

#include <tnrp/TnrdReader.h>

#include "SessionModel.h"

// Qt playback driver on top of libtnrp's tnrp::TnrdReader. The reader owns all
// .tnrd I/O — gzip decompression, the time/type index, per-lap blocks and the
// snapshot/streaming reads — so this class is just the clock: it scans the whole
// recording into a SessionData once (for the lap-aware charts), then advances a
// QTimer-driven cursor, pulling the due rows from the reader and emitting them.
//
// Public interface and signals are unchanged from the old self-contained player,
// so MainWindow's playback wiring needs no changes.
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
    void seekToTime(float absoluteTime);
    void setSpeed(float mult);

    float totalTime()   const { return totalTime_; }
    float currentTime() const { return currentTime_; }
    bool  isPlaying()   const { return playing_; }

    // The full session built during the load scan, moved into SessionModel by the
    // UI once `loaded` fires. Valid only on the main thread after that signal.
    SessionData takeScannedData() { return std::move(scanned_); }

signals:
    void loadingStarted();
    void loaded(nlohmann::json header);
    void loadFailed();
    void packetReady(nlohmann::json packet);
    void stateChanged(bool playing, float currentTime, float totalTime, float speed);
    void seeked();
    void finished();

private slots:
    void tick();

private:
    tnrp::TnrdReader reader_;   // owns the decompressed temp file + index; loaded on the bg thread
    SessionData      scanned_;  // built on the load thread from reader_.readRange()

    float        startTime_   = 0.0f;
    float        totalTime_   = 0.0f;
    float        currentTime_ = 0.0f;
    float        speed_       = 1.0f;
    bool         playing_     = false;
    bool         loading_     = false;

    std::atomic<bool> cancelled_{false};

    QTimer*       timer_       = nullptr;
    QElapsedTimer elapsed_;

    // Scans every row of the loaded recording into `scanned_` so the charts have
    // the whole session (telemetry/status/lap/motion/motion_ex/tyre). Runs on the
    // load thread. Mirrors the old buildIndex() second pass.
    void scanIntoSessionData();

    // Parse a raw JSONL row and emit it as a packetReady() for the live panels.
    void emitRows(const std::vector<std::string>& rows);
    void emitState();
    void cleanup();
};
