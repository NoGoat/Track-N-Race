#pragma once

#include <array>
#include <limits>
#include <atomic>
#include <cstddef>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
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

namespace detail { class LiveV6Store; class TnrdV6Archive; }

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

    // The live session's V6 store, which is the recorder's V6 writer kept in
    // memory (TnrdWriter::setRetainSession). Its bytes are already part of
    // writerMemoryStats().retainedBytes, so retainedBytes here stays 0 and
    // nothing is counted twice.
    struct LiveHistoryMemoryStats {
        size_t retainedBytes{};
        bool sessionRetained{};
        bool fileAttached{};
        size_t builderCount{};
        size_t builderBytes{};
        size_t builderCapacityBytes{};
        size_t pendingLapCount{};
        size_t pendingLapBytes{};
        size_t committedLapCount{};
        size_t chunkCount{};
        size_t chunkBytes{};
        size_t sharedRecords{};
        size_t sharedBytes{};
        uint64_t uncommittedLaps{};
        uint64_t chunkWrites{};
        uint64_t chunkPlainBytesProcessed{};
        uint64_t chunkCompressedBytes{};
        size_t compressionScratchCapacityBytes{};
        size_t compressionContextBytes{};
        size_t queuedJobs{};
        size_t queuedRows{};
        size_t queuedRowBytes{};
        size_t queuedReads{};
        uint64_t imagesReceived{};
        uint64_t readsCompleted{};
        size_t lastImageEncodedBytes{};
        size_t peakImageEncodedBytes{};
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
    // Which drivers each session category records, from the next recording.
    void setRecordingScopes(const RecordingScopes& scopes);
    // Blocks until queued recording rows and the rolling buffer have reached a
    // recoverable codec/stdio flush point. Used before playback and by host
    // shutdown/crash hooks.
    void flushRecording();

    // One renderer-wide subscription, aggregated from the active page and its
    // visible sections. Recording remains complete; these masks only control
    // consumer forwarding, playback chunk loading, and history backfill.
    void requestDataRequirements(uint64_t requestId);
    void setDataRequirements(uint32_t streamRowMask, uint32_t historyRowMask,
                             float windowSeconds, uint64_t requestId = 0,
                             const std::vector<uint8_t>& v6Types = {},
                             const std::vector<uint8_t>& v6HistoryTypes = {});

    // ── Paired displays ──────────────────────────────────────────────────
    // The transport, authentication, discovery, subscriptions and latest-row
    // cache all live in libtnrp so every host gets identical behaviour.
    bool pairStart(std::string* errorOut = nullptr);
    void pairStop(bool persistDisabled = true);
    void pairOpenWindow();
    void pairCloseWindow();
    // Allows or refuses the phone waiting for approval (pendingDevice).
    void pairRespond(bool approve);
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
    void playerSetDriver(int driverIndex, bool useRecordedRows = false);
    // V6 playback: the car selected in Standings, whose private status (ERS,
    // fuel, aero, brake bias) is read and streamed. The rest of the grid
    // streams only lap timing and tyre state. -1 clears it; live is unaffected.
    void playerSetFocusDriver(int driverIndex);
    void playerGetLapData(int lapNum, uint32_t rowTypeMask = 0xFFFFFFFFu);
    std::string playerGetAnalysisLapData(int lapNum, uint32_t rowTypeMask,
                                         int driverIndex) const;
    // Answers a live lap request: a live_lap_data / live_fastest_lap_data JSON
    // header (request id, lap number, lap time, start and end session times)
    // and the lap's chart families as a V6H1 payload. Called on the live
    // store's read thread; never called when there is no such lap.
    using LiveLapCallback = std::function<void(std::string headerJson,
                                               std::shared_ptr<std::vector<uint8_t>> columnar)>;
    // The player's fastest completed lap from the live V6 store.
    void liveGetFastestLap(uint64_t requestId, LiveLapCallback done);
    // One of the player's laps from the live V6 store: the committed lap
    // decompressed, or the lap still inside the write delay read from its
    // builders. Live Previous and Fastest read their laps this way.
    void liveGetLapData(uint64_t requestId, int lapNum, LiveLapCallback done);
    // The car whose lap-times view is open (-1 when none). While set, the
    // engine emits that car's driver_lap_history row now and again only when
    // it changes: a completed lap (live), the cursor passing a lap end or a
    // seek (playback), or a new recording. Nothing polls for it.
    void setLapHistoryCar(int carIdx);
    bool playerGetAnalysisLapProgress(int lapNum, AnalysisLapProgress& out,
                                      int driverIndex = -1) const;
    void playerGetAllLapsData(uint64_t requestId = 0, uint32_t rowTypeMask = 0xFFFFFFFFu);
    void playerGetWindowData(float windowSeconds, uint64_t requestId = 0,
                             uint32_t rowTypeMask = 0xFFFFFFFFu);
    void playerClose();                    // back to live mode

    // Host window visibility. A hidden host drops the rows it is sent, so on
    // the hidden-to-visible edge the engine rebuilds what it missed from its
    // own history (live) or the recording (playback) and sends one
    // Sink::onRestoreFlush. sequence orders calls that reach the engine on
    // different threads; an older call is ignored. Showing may read the
    // recording, so call it off the UI thread.
    void setHostVisible(bool visible, uint64_t sequence);

private:
    void emitRows(const std::vector<std::string>& rows);

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
    uint64_t          appliedRequirementsRequestId_ = 0; // guarded by mutex_
    std::condition_variable requirementsCv_;
    std::thread       playThread_;
    std::atomic<bool> playRun_{false};

    // Columnar V6 history (Config::columnarV6History). A second read-only
    // handle on the loaded recording, so seek and All Laps extraction run
    // without mutex_ and never contend with the playback thread's archive
    // cursor or file handle. historyMutex_ serializes use of that handle and
    // is never taken while mutex_ is held in the other order.
    std::mutex historyMutex_;
    std::unique_ptr<detail::TnrdV6Archive> historyArchive_; // guarded by historyMutex_
    std::atomic<bool> historyArchiveReady_{false};
    // Bumped when the timeline a history read was taken against stops being
    // valid (driver switch, load, close). Reads compare it to their snapshot.
    std::atomic<uint64_t> historyEpoch_{0};
    struct V6HistoryRead {
        uint8_t driver = 0;
        std::vector<uint8_t> types;
        uint32_t mask = 0;
        float from = 0.0f;
        float to = 0.0f;
        bool seed = false;
        uint64_t epoch = 0;
    };
    // mutex_ held. True when this read should take the columnar path.
    bool prepareV6HistoryReadLocked(float from, float to, uint32_t mask, V6HistoryRead& out) const;
    // mutex_ NOT held. Null when cancelled or superseded; an empty buffer when
    // the read failed, so the caller still answers the request. An
    // authoritative seek ignores the epoch: the renderer is blocked on its
    // flush, and a driver change is always followed by a newer seek anyway.
    std::shared_ptr<std::vector<uint8_t>> runV6HistoryRead(const V6HistoryRead& read,
                                                           const std::function<bool()>& cancelled,
                                                           bool respectEpoch);

    // Binary-playback sparse-row cache (guarded by mutex_): the last seen raw
    // line per panel type. Damage is cached for initial/seek restoration but is
    // streamed from TnrdReader's 10 Hz reconstruction; other dup types are
    // re-emitted with session_time set to the playhead between native updates.
    std::array<std::string, 16> dupCache_{};
    std::array<std::string, 16> liveLatestRows_{};
    // V6 playback's derived fastest-lap holder last sent (-2 = none sent).
    int               playbackFastestLapCar_ = -2;
    // The fastest_lap row for the cursor when `force` or when the holder has
    // changed; empty otherwise, and always for non-V6 recordings.
    std::string playbackFastestLapRowLocked(bool force);
    // Latest session_history_fastest row per car for the live session, which
    // carries that car's whole lap list.
    std::array<std::string, 24> liveLapHistoryRows_{};
    uint64_t          liveLapHistorySessionUid_ = 0;
    // Header m_sessionUID of the live session the history store holds.
    uint64_t          liveSessionUid_ = 0;
    bool              liveSessionUidKnown_ = false;
    int               lapHistoryCar_ = -1;
    std::string       lastLapHistoryJson_;
    // Playback: the next lap end after the cursor, when the row may change.
    float             nextLapHistoryCheck_ = std::numeric_limits<float>::infinity();
    DriverLapHistoryRow driverLapHistoryLocked(int carIdx) const;
    // The row when `force` or when it differs from the last one sent; empty
    // when no car is subscribed.
    std::string lapHistoryRowLocked(bool force);
    std::string lastStrategyJson_;
    struct LiveJsonHistoryRow {
        float sessionTime{};
        uint64_t sequence{};
        std::shared_ptr<const std::string> json;
    };
    // Reads of the live session, which writer_'s one V6 writer holds as a
    // TNRD V6 file holds it: every car, by lap and data type
    // (Config::binaryPlayback hosts only).
    std::unique_ptr<detail::LiveV6Store> liveV6_;
    uint64_t          liveStrategySequence_ = 0;
    // The player's lap starts, for putting the lap back after a rewind.
    std::map<int, float> liveLapStarts_;
    // Event vehicle indices are uint8. A finite entry means that car already
    // retired on the surviving live timeline.
    std::array<float, 256> liveRetirementTimes_{};
    float             liveSessionTime_ = 0.0f;
    float             liveLapStart_ = 0.0f;
    int               liveLapNum_ = 0;
    uint32_t          consumerRowMask_ = 0xFFFFFFFFu;
    uint32_t          consumerHistoryMask_ = 0;
    float             consumerWindowSeconds_ = 0.0f;
    uint32_t          hostConsumerRowMask_ = 0xFFFFFFFFu;
    uint32_t          hostConsumerHistoryMask_ = 0;
    float             hostConsumerWindowSeconds_ = 0.0f;
    std::vector<uint8_t> hostConsumerV6Types_;
    std::vector<uint8_t> hostConsumerV6HistoryTypes_;
    uint32_t          pairConsumerRowMask_ = 0;
    // Union of the connected phones' V6 field requests (guarded by mutex_).
    // Phones never request history, so there is no pair history list.
    std::vector<uint8_t> pairConsumerV6Types_;
    // Last "driver_restriction" row emitted, so a moving cursor crossing a
    // change re-states it and an unchanged setting stays silent (guarded by
    // mutex_). Cleared on playback open/close.
    std::string       lastDriverRestriction_;

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
    // The last values the diagnostic getters read under mutex_. Electron polls
    // them on its main thread, so while a seek holds mutex_ they answer from
    // here instead of stalling every window until it finishes.
    struct StrategyLockedStats {
        bool subscribed{};
        size_t cacheCapacityBytes{};
        StrategyProcessor::MemoryStats processor;
    };
    mutable std::mutex diagnosticSnapshotMutex_;
    mutable LiveDiagnostics lastLiveDiagnostics_;
    mutable std::string lastUdpError_;
    mutable RuntimeMemoryStats lastRuntimeMemoryStats_;
    mutable StrategyLockedStats lastStrategyLockedStats_;
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
    // A new live session UID: drop the previous session's history so a
    // backfill or restore never returns its rows. mutex_ held.
    void resetLiveSessionHistoryLocked();

    // Host restore state (guarded by mutex_). See setHostVisible().
    uint64_t          hostVisibilitySequence_ = 0;
    bool              hostHidden_ = false;
    // Earliest time the host may be missing rows from. Starts at the hide
    // time and moves back with every rewind, seek or load while hidden.
    float             hiddenRestoreFrom_ = 0.0f;
    uint64_t          hiddenSessionUid_ = 0;
    bool              hiddenSessionUidKnown_ = false;
    // A live restore issued but not yet handed to the sink. A rewind while it
    // is pending moves its start back; hiding again folds it into the new gap.
    uint64_t          restoreGeneration_ = 0;
    bool              restorePending_ = false;
    float             pendingRestoreFrom_ = 0.0f;
    bool              pendingSessionChanged_ = false;
    void noteHostTimelineMovedLocked(float sessionTime);
    // Latest row of each current-state family the host subscribes to.
    std::vector<std::string> hostLatestRowsLocked();
    void issueLiveRestoreLocked();
    void runPlaybackRestore(float restoreFrom);
    void emitRow(const std::string& json);               // forward to the sink
    void emitBinary(const uint8_t* data, size_t length);
    void setPairDataRequirements(uint32_t streamRowMask,
                                 const std::vector<uint8_t>& v6Types,
                                 bool refreshSnapshot);
    // Shared body of setDataRequirements()/setPairDataRequirements().
    // forceRestoreMask re-emits the latest state of those row families even when
    // they were already enabled (a newly subscribed phone needs its own
    // baseline); pairInitiated marks calls that must not re-prime the reader
    // when nothing it loads has changed.
    void applyDataRequirements(uint32_t streamRowMask, uint32_t historyRowMask,
                               float windowSeconds, uint64_t requestId,
                               const std::vector<uint8_t>& v6Types,
                               const std::vector<uint8_t>& v6HistoryTypes,
                               uint32_t forceRestoreMask, bool pairInitiated);
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
