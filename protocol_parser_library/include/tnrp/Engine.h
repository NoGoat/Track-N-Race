#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

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
    void restartUdp(uint16_t port, const std::string& bindAddress);

    // ── Live config ──────────────────────────────────────────────────────
    void setOverride(Override ovr);
    void setLogging(bool enabled, const std::string& outputDir);

    // ── Playback ─────────────────────────────────────────────────────────
    // Loads a .tnrd, switches the engine into playback mode (UDP ignored),
    // emits the initial reconstructed snapshot, and stays paused.
    bool playerLoad(const std::string& path);
    void playerPlay();
    void playerPause();
    void playerSeek(float pct);            // 0..1 of the recording duration
    void playerSetSpeed(float mult);
    void playerGetLapData(int lapNum);
    void playerClose();                    // back to live mode

private:
    Config        config_;
    Sink*         sink_;
    Parser        parser_;
    TnrdWriter    writer_;
    TnrdReader    reader_;
    UdpListener   udp_;

    std::mutex    mutex_;                  // guards all mutable state below

    // Playback clock state.
    std::atomic<bool> inPlayback_{false};
    bool              playing_   = false;
    float             currentTime_ = 0.0f;  // absolute session_time cursor
    float             speed_     = 1.0f;
    std::thread       playThread_;
    std::atomic<bool> playRun_{false};

    void onDatagram(const uint8_t* data, int length);   // UDP receive thread
    void emitRow(const nlohmann::json& row);            // forward to the sink
    void playbackLoop();                                 // playback thread body
    void stopPlaybackThread();
    void emitPlaybackState();
};

} // namespace tnrp
