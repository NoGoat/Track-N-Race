#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "tnrp/Config.h"
#include "tnrp/Parser.h"
#include "tnrp/PairServer.h"
#include "tnrp/Sink.h"
#include "tnrp/Strategy.h"
#include "tnrp/TnrdReader.h"
#include "tnrp/TnrdWriter.h"
#include "tnrp/UdpListener.h"

namespace tnrp {

namespace detail { class LiveHistoryStore; }

// Orchestrates the whole telemetry pipeline and is the only class consumers
// (the bridge, later the native app) construct directly. It wires:
//
//   UdpListener --datagram--> Parser --rows--> TnrdWriter (record)
//                                          \--> Sink (forward to consumer)
//   TnrdReader  --playback rows-----------------> Sink
//
// Live and playback are mutually exclusive: while a clip is loaded, incoming UDP
// datagrams are dropped (mirrors the native recorder's inPlayback_ behaviour).
//
// All state transitions are guarded by a single mutex so the UDP receive thread,
// the playback thread and control calls (setOverride/setLogging/player*) can run
// concurrently.
class Engine {
public:
    struct LiveDiagnostics {
        bool udpRunning = false;
        bool inPlayback = false;
        bool recording = false;
        uint64_t datagrams = 0;
        uint64_t bytes = 0;
        uint64_t tooShort = 0;
        uint64_t unsupportedFormat = 0;
        uint64_t parserDropped = 0;
        uint64_t accepted = 0;
        uint64_t rowsProduced = 0;
        uint64_t binaryBytesProduced = 0;
        uint64_t noOutput = 0;
        std::array<uint64_t, 18> packetIds{}; // 0..16 plus an "other" bucket
        uint64_t format2024 = 0;
        uint64_t format2025 = 0;
        uint64_t format2026 = 0;
        uint16_t lastIncomingFormat = 0;
        uint8_t lastPacketId = 0;
        int lastDatagramLength = 0;
        float lastSessionTime = 0.0f;
        uint32_t consumerRowMask = 0;
        uint32_t consumerHistoryMask = 0;
        float consumerWindowSeconds = 0.0f;
    };

    struct LiveHistoryMemoryStats {
        size_t retainedBytes{};
        size_t lapCount{};
        size_t pinnedLapCount{};
        size_t compressedLapCount{};
        size_t busyLapCount{};
        size_t packedBytes{};
        size_t packedCapacityBytes{};
        size_t jsonRows{};
        size_t jsonPayloadBytes{};
        size_t jsonPayloadCapacityBytes{};
        size_t jsonContainerCapacityBytes{};
        size_t sequenceEntries{};
        size_t sequenceCapacityBytes{};
        size_t compressedPlainBytes{};
        size_t compressedBytes{};
        size_t compressedCapacityBytes{};
        size_t queuedJobs{};
        int activeJobKind{};
        uint64_t compressionJobs{};
        uint64_t compressedFamilies{};
        uint64_t compressionPlainBytesProcessed{};
        uint64_t compressionPlainBufferBytesAllocated{};
        uint64_t compressionBufferBytesAllocated{};
        uint64_t compressedOutputBytesAllocated{};
        size_t lastCompressionPlainBytes{};
        size_t lastCompressionBufferBytes{};
        size_t lastCompressionScratchBytes{};
        size_t peakCompressionPlainBytes{};
        size_t peakCompressionBufferBytes{};
        size_t peakCompressionScratchBytes{};
        uint64_t decompressionJobs{};
        uint64_t decompressionBufferBytesAllocated{};
        size_t lastDecompressionBufferBytes{};
        size_t peakDecompressionBufferBytes{};
        uint64_t rangeJobs{};
    };

    struct StrategyRollbackMemoryStats {
        size_t retainedBytes{}, checkpoints{}, rows{};
        uint64_t rollbacks{}, replayedRows{}, fallbacks{};
    };
    struct StrategyMemoryStats {
        bool subscribed{};
        size_t retainedBytes{};
        size_t cacheCapacityBytes{};
        size_t queuedWorkItems{};
        size_t queuedRows{};
        size_t queuedJsonBytes{};
        size_t queuedRetainedBytes{};
        uint64_t oldestQueuedWorkAgeMs{};
        size_t peakQueuedRows{};
        size_t peakQueuedRetainedBytes{};
        size_t activeWorkItems{};
        size_t activeRows{};
        size_t activeJsonBytes{};
        size_t activeRetainedBytes{};
        uint64_t inputRowsEnqueued{};
        uint64_t inputJsonBytesEnqueued{};
        uint64_t rowsProcessed{};
        uint64_t jsonBytesProcessed{};
        uint64_t snapshotsGenerated{};
        uint64_t snapshotsEmitted{};
        uint64_t snapshotJsonBytesGenerated{};
        size_t lastSnapshotJsonBytes{};
        size_t lastSnapshotJsonCapacityBytes{};
        size_t peakSnapshotJsonBytes{};
        StrategyProcessor::MemoryStats processor;
        StrategyRollbackMemoryStats rollback;
    };

    struct RuntimeMemoryStats {
        size_t retainedBytes{};
        size_t duplicateCacheUsedBytes{};
        size_t duplicateCacheCapacityBytes{};
        size_t latestRowCacheUsedBytes{};
        size_t latestRowCacheCapacityBytes{};
        size_t playbackPathCapacityBytes{};
        uint64_t datagramsProcessed{};
        uint64_t datagramBytesProcessed{};
        uint64_t parserRowsProduced{};
        uint64_t parserControlRowsProduced{};
        uint64_t parserHotJsonRowsProduced{};
        uint64_t parserJsonBytesProduced{};
        uint64_t parserBinaryBytesProduced{};
        uint64_t parserResultCapacityBytesAllocated{};
        size_t lastParserResultCapacityBytes{};
        size_t peakParserResultCapacityBytes{};
        uint64_t filteredBinaryBatches{};
        uint64_t filteredBinaryBytesProduced{};
        uint64_t filteredBinaryCapacityBytesAllocated{};
        size_t lastFilteredBinaryCapacityBytes{};
        size_t peakFilteredBinaryCapacityBytes{};
    };

    Engine(const Config& config, Sink* sink);
    ~Engine();

    // ── Live ─────────────────────────────────────────────────────────────
    bool startUdp();                       // bind + begin receiving
    bool restartUdp(uint16_t port, const std::string& bindAddress);
    std::string udpLastError() const;
    void setDiagnosticsEnabled(bool enabled);
    LiveDiagnostics liveDiagnostics() const;
    LiveHistoryMemoryStats liveHistoryMemoryStats() const;
    StrategyMemoryStats strategyMemoryStats() const;
    RuntimeMemoryStats runtimeMemoryStats() const;
    TnrdWriter::MemoryStats writerMemoryStats() const;

    // ── Live config ──────────────────────────────────────────────────────
    void setOverride(Override ovr);
    void setTeamColorOverrides(TeamColorOverrides overrides);
    std::string teamColorCatalogJson() const;
    void setStrategyMinimumStops(int stops);
    void setLogging(bool enabled, const std::string& outputDir);
    void setLoggingZstd(bool enabled, const std::string& outputDir);
    [[deprecated("TNRD V1/gzip writing is retained only for compatibility; use setLoggingZstd")]]
    void setLoggingGzip(bool enabled, const std::string& outputDir);
    // Blocks until queued recording rows and the rolling buffer have reached a
    // recoverable codec/stdio flush point. Used before playback and by host
    // shutdown/crash hooks.
    void flushRecording();

    // One renderer-wide subscription, aggregated from the active page and its
    // visible sections. Recording remains complete; these masks only control
    // consumer forwarding, playback chunk loading, and history backfill.
    void requestDataRequirements(uint64_t requestId);
    void setDataRequirements(uint32_t streamRowMask, uint32_t historyRowMask,
                             float windowSeconds, uint64_t requestId = 0);

    // ── Paired displays ──────────────────────────────────────────────────
    // The transport, authentication, discovery, subscriptions and latest-row
    // cache all live in libtnrp so every host gets identical behaviour.
    bool pairStart(std::string* errorOut = nullptr);
    void pairStop(bool persistDisabled = true);
    void pairOpenWindow();
    void pairCloseWindow();
    void pairRemoveDevice(const std::string& id);
    std::string pairStateJson() const;
    std::string pairPersistedStateJson() const;

    // ── Playback ─────────────────────────────────────────────────────────
    // Loads a .tnrd, switches the engine into playback mode (UDP ignored),
    // emits the initial reconstructed snapshot, and stays paused.
    bool playerLoad(const std::string& path, std::string* errorOut = nullptr);
    void playerPlay();
    void playerPause();
    // Register on the caller thread before an async seek worker is queued.
    // Playback remains gated until that exact generation is applied.
    void playerRequestSeek(uint64_t requestId);
    void playerSeek(float pct, bool allHistory = false, uint64_t requestId = 0,
                    uint32_t rowTypeMask = 0xFFFFFFFFu, float windowSeconds = 0.0f);
    void playerSetSpeed(float mult);
    void playerGetLapData(int lapNum, uint32_t rowTypeMask = 0xFFFFFFFFu);
    void liveGetFastestLap(uint64_t requestId);
    bool playerGetAnalysisLapProgress(int lapNum, AnalysisLapProgress& out) const;
    void playerGetAllLapsData(uint64_t requestId = 0, uint32_t rowTypeMask = 0xFFFFFFFFu);
    void playerGetWindowData(float windowSeconds, uint64_t requestId = 0,
                             uint32_t rowTypeMask = 0xFFFFFFFFu);
    void playerClose();                    // back to live mode

private:
    Config        config_;
    Sink*         sink_;
    Parser        parser_;
    TnrdWriter    writer_;
    // Playback ticks own this reducer after an asynchronous strategy rebuild
    // has committed. The strategy worker below reconstructs both live and
    // playback snapshots so neither UDP ingest nor the playback clock performs
    // a full historical replay.
    StrategyProcessor strategy_;
    TnrdReader    reader_;
    UdpListener   udp_;
    PairServer    pairServer_;

    mutable std::mutex mutex_;             // guards all mutable state below

    // Playback clock state.
    std::atomic<bool> inPlayback_{false};
    std::atomic<uint16_t> emittedFormat_{0};
    std::shared_ptr<const TeamColorOverrides> teamColorOverrides_;
    bool              playing_   = false;
    float             currentTime_ = 0.0f;  // absolute session_time cursor
    float             speed_     = 1.0f;
    std::atomic<uint64_t> latestSeekRequestId_{0};
    std::atomic<uint64_t> latestRequirementsRequestId_{0};
    uint64_t          appliedSeekRequestId_ = 0; // guarded by mutex_
    std::thread       playThread_;
    std::atomic<bool> playRun_{false};

    // Binary-playback sparse-row cache (guarded by mutex_): the last seen raw
    // line per panel type. Damage is cached for initial/seek restoration but is
    // streamed from TnrdReader's 10 Hz reconstruction; other dup types are
    // re-emitted with session_time set to the playhead between native updates.
    std::array<std::string, 16> dupCache_{};
    std::array<std::string, 16> liveLatestRows_{};
    std::string lastStrategyJson_;
    struct LiveJsonHistoryRow {
        float sessionTime{};
        uint64_t sequence{};
        std::shared_ptr<const std::string> json;
    };
    std::unique_ptr<detail::LiveHistoryStore> liveHistory_;
    uint64_t          liveHistorySequence_ = 0;
    std::array<float, 16> liveHistoryLastSample_{};
    std::array<int, 16> liveHistoryLastLap_{};
    float             liveSessionTime_ = 0.0f;
    float             liveLapStart_ = 0.0f;
    int               liveLapNum_ = 0;
    uint32_t          consumerRowMask_ = 0xFFFFFFFFu;
    uint32_t          consumerHistoryMask_ = 0;
    float             consumerWindowSeconds_ = 0.0f;
    uint32_t          hostConsumerRowMask_ = 0xFFFFFFFFu;
    uint32_t          hostConsumerHistoryMask_ = 0;
    float             hostConsumerWindowSeconds_ = 0.0f;
    uint32_t          pairConsumerRowMask_ = 0;

    enum class StrategyWorkKind { Update, Rollback, Reset, Configure, PlaybackRebuild };
    struct StrategyWork {
        StrategyWorkKind kind{StrategyWorkKind::Update};
        uint64_t generation{};
        uint16_t format{2025};
        int minimumStops{};
        bool forceSnapshot{};
        std::vector<LiveJsonHistoryRow> rows;
        float rebuildThrough{};
        uint64_t seekRequestId{};
        std::string playbackPath;
        uint64_t queuedAtMs{};
    };
    mutable std::mutex strategyWorkMutex_;
    std::condition_variable strategyWorkCv_;
    std::deque<StrategyWork> strategyWorkQueue_;
    std::thread strategyThread_;
    bool strategyStop_ = false;
    mutable std::mutex strategyMemoryStatsMutex_;
    StrategyProcessor::MemoryStats publishedLiveStrategyMemoryStats_;
    StrategyRollbackMemoryStats publishedStrategyRollbackMemoryStats_;
    std::atomic<size_t> strategyPeakQueuedRows_{0};
    std::atomic<size_t> strategyPeakQueuedRetainedBytes_{0};
    std::atomic<size_t> strategyActiveWorkItems_{0};
    std::atomic<size_t> strategyActiveRows_{0};
    std::atomic<size_t> strategyActiveJsonBytes_{0};
    std::atomic<size_t> strategyActiveRetainedBytes_{0};
    std::atomic<uint64_t> strategyInputRowsEnqueued_{0};
    std::atomic<uint64_t> strategyInputJsonBytesEnqueued_{0};
    std::atomic<uint64_t> strategyRowsProcessed_{0};
    std::atomic<uint64_t> strategyJsonBytesProcessed_{0};
    std::atomic<uint64_t> strategySnapshotsGenerated_{0};
    std::atomic<uint64_t> strategySnapshotsEmitted_{0};
    std::atomic<uint64_t> strategySnapshotJsonBytesGenerated_{0};
    std::atomic<size_t> strategyLastSnapshotJsonBytes_{0};
    std::atomic<size_t> strategyLastSnapshotJsonCapacityBytes_{0};
    std::atomic<size_t> strategyPeakSnapshotJsonBytes_{0};
    uint64_t liveStrategyGeneration_ = 1; // guarded by mutex_
    uint16_t liveStrategyFormat_ = 2025;  // guarded by mutex_
    std::atomic<uint64_t> playbackStrategyGeneration_{0};
    uint64_t playbackStrategyPendingGeneration_ = 0; // guarded by mutex_
    bool playbackStrategyPending_ = false;           // guarded by mutex_
    std::vector<std::string> playbackStrategyPendingRows_; // guarded by mutex_
    std::string playbackPath_;                        // guarded by mutex_
    uint64_t runtimeDatagramsProcessed_{};             // guarded by mutex_
    uint64_t runtimeDatagramBytesProcessed_{};         // guarded by mutex_
    uint64_t runtimeParserRowsProduced_{};             // guarded by mutex_
    uint64_t runtimeParserControlRowsProduced_{};      // guarded by mutex_
    uint64_t runtimeParserHotJsonRowsProduced_{};      // guarded by mutex_
    uint64_t runtimeParserJsonBytesProduced_{};        // guarded by mutex_
    uint64_t runtimeParserBinaryBytesProduced_{};      // guarded by mutex_
    uint64_t runtimeParserResultCapacityAllocated_{};  // guarded by mutex_
    size_t runtimeLastParserResultCapacity_{};         // guarded by mutex_
    size_t runtimePeakParserResultCapacity_{};         // guarded by mutex_
    uint64_t runtimeFilteredBinaryBatches_{};          // guarded by mutex_
    uint64_t runtimeFilteredBinaryBytesProduced_{};    // guarded by mutex_
    uint64_t runtimeFilteredBinaryCapacityAllocated_{};// guarded by mutex_
    size_t runtimeLastFilteredBinaryCapacity_{};       // guarded by mutex_
    size_t runtimePeakFilteredBinaryCapacity_{};       // guarded by mutex_
    bool              liveDiagnosticsEnabled_ = false;
    LiveDiagnostics   liveDiagnostics_{};

    void onDatagram(const uint8_t* data, int length);   // UDP receive thread
    void rewindLiveTimeline(float sessionTime, uint16_t format); // mutex_ held
    void emitRow(const std::string& json);               // forward to the sink
    void emitBinary(const uint8_t* data, size_t length);
    void setPairDataRequirements(uint32_t streamRowMask);
    void ingestStrategyRow(const std::string& json);
    void emitStrategy(bool force = false);
    void enqueueLiveStrategyWork(StrategyWork work);
    void strategyLoop();
    void stopStrategyThread();
    // Starts a generation-controlled rebuild on strategyThread_. Call with
    // mutex_ held. Playback continues while dependency rows are accumulated and
    // folded into the rebuilt processor immediately before it commits.
    bool preparePlaybackStrategyRebuildLocked(float target, StrategyWork& work);
    void requestPlaybackStrategyRebuildLocked(float target);
    void playbackLoop();                                 // playback thread body
    void stopPlaybackThread();
    void emitPlaybackState();
};

} // namespace tnrp
