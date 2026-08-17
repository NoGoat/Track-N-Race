#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "tnrp/Config.h"
#include "tnrp/Parser.h"
#include "tnrp/Sink.h"
#include "tnrp/TnrdReader.h"
#include "tnrp/TnrdWriter.h"
#include "tnrp/UdpListener.h"

namespace tnrp {

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
    void playerGetAllLapsData(uint64_t requestId = 0, uint32_t rowTypeMask = 0xFFFFFFFFu);
    void playerGetWindowData(float windowSeconds, uint64_t requestId = 0,
                             uint32_t rowTypeMask = 0xFFFFFFFFu);
    void playerClose();                    // back to live mode

private:
    Config        config_;
    Sink*         sink_;
    Parser        parser_;
    TnrdWriter    writer_;
    TnrdReader    reader_;
    UdpListener   udp_;

    mutable std::mutex mutex_;             // guards all mutable state below

    // Playback clock state.
    std::atomic<bool> inPlayback_{false};
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
    struct LivePackedIndex { float sessionTime{}; size_t offset{}; size_t length{}; };
    struct LivePackedHistory {
        std::vector<uint8_t> bytes;
        std::deque<LivePackedIndex> index;
        size_t discardedBytes = 0;
    };
    struct LiveJsonHistoryRow { float sessionTime{}; std::string json; };
    std::array<LivePackedHistory, 16> livePackedHistory_{};
    std::array<std::deque<LiveJsonHistoryRow>, 16> liveJsonHistory_{};
    float             liveSessionTime_ = 0.0f;
    float             liveLapStart_ = 0.0f;
    int               liveLapNum_ = 0;
    uint32_t          consumerRowMask_ = 0xFFFFFFFFu;
    uint32_t          consumerHistoryMask_ = 0;
    float             consumerWindowSeconds_ = 0.0f;

    void onDatagram(const uint8_t* data, int length);   // UDP receive thread
    void rewindLiveTimeline(float sessionTime);          // mutex_ held
    void emitRow(const std::string& json);               // forward to the sink
    void playbackLoop();                                 // playback thread body
    void stopPlaybackThread();
    void emitPlaybackState();
};

} // namespace tnrp
