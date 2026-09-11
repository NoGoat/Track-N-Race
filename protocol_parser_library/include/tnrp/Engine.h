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
    Engine(const Config& config, Sink* sink);
    ~Engine();

    // ── Live ─────────────────────────────────────────────────────────────
    bool startUdp();                       // bind + begin receiving
    bool restartUdp(uint16_t port, const std::string& bindAddress);
    std::string udpLastError() const;

    // ── Live config ──────────────────────────────────────────────────────
    void setOverride(Override ovr);
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
    // Playback owns this reducer on the playback/load/seek workers. Live
    // strategy has its own worker below so UDP receive never performs strategy
    // parsing, calculation, or flashback reconstruction.
    StrategyProcessor strategy_;
    TnrdReader    reader_;
    UdpListener   udp_;
    PairServer    pairServer_;

    mutable std::mutex mutex_;             // guards all mutable state below

    // Playback clock state.
    std::atomic<bool> inPlayback_{false};
    bool              playing_   = false;
    float             currentTime_ = 0.0f;  // absolute session_time cursor
    float             speed_     = 1.0f;
    std::atomic<uint64_t> latestSeekRequestId_{0};
    std::atomic<uint64_t> latestRequirementsRequestId_{0};
    uint64_t          appliedSeekRequestId_ = 0; // guarded by mutex_
    bool              strategyRebuildPending_ = false;
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

    enum class StrategyWorkKind { Update, Rebuild, Reset, Configure };
    struct StrategyWork {
        StrategyWorkKind kind{StrategyWorkKind::Update};
        uint64_t generation{};
        uint16_t format{2025};
        int minimumStops{};
        bool forceSnapshot{};
        std::vector<LiveJsonHistoryRow> rows;
        float rebuildThrough{};
    };
    std::mutex strategyWorkMutex_;
    std::condition_variable strategyWorkCv_;
    std::deque<StrategyWork> strategyWorkQueue_;
    std::thread strategyThread_;
    bool strategyStop_ = false;
    uint64_t liveStrategyGeneration_ = 1; // guarded by mutex_
    uint16_t liveStrategyFormat_ = 2025;  // guarded by mutex_

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
    // Rebuilds and commits the derived playback strategy while mutex_ is held.
    // The returned row is emitted only after the caller releases the lock.
    std::string rebuildPlaybackStrategyLocked(float target);
    void playbackLoop();                                 // playback thread body
    void stopPlaybackThread();
    void emitPlaybackState();
};

} // namespace tnrp
