#pragma once

#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace tnrp {

// Reads a .tnrd file for playback. Qt-free port of the indexing core of the
// native TnrdPlayer + the Electron sessionPlayer scan: decompress to a temp
// file, build a time/type index, and — in the same pass — build the per-lap
// Speed/RPM/ERS comparison blocks, the scanned lap list and the event log that
// the renderer needs to enter true "playback mode" (see playback_lap_blocks).
//
// Without those, the renderer treats playback as live and re-filters an
// ever-growing buffer every frame (progressive slowdown) and can't show history
// on seek. The engine emits lapBlocksMessage() on load and a seek flush built
// from readRange() on seek.
//
// Not thread-safe; the engine serializes access from its playback thread.
class TnrdReader {
public:
    TnrdReader() = default;
    ~TnrdReader();

    bool load(const std::string& path, nlohmann::json& outHeader);
    void close();
    bool isLoaded() const { return tempFile_ != nullptr; }

    float startTime() const { return startTime_; }
    float totalTime() const { return totalTime_; }

    // ── Playback streaming (clock owned by the engine) ───────────────────────
    void setCursor(float t);                              // playPos_ = first index > t
    std::vector<nlohmann::json> pullUntil(float t);       // rows with sessionTime <= t from cursor
    std::vector<nlohmann::json> drainRest();              // remaining rows
    bool hasMore() const { return playPos_ < index_.size(); }

    // ── Seek support ─────────────────────────────────────────────────────────
    // Most recent state row (session/timing/participants/all_status/tyre_sets)
    // at or before t, in chronological order. These are panel-state setters, not
    // buffers, so they're emitted individually (cf. broadcastInitialState).
    std::vector<nlohmann::json> stateSnapshot(float t);
    // All rows whose sessionTime is within [fromTime, toTime] (one contiguous
    // read). Used to fill the renderer's dense buffers on seek.
    std::vector<nlohmann::json> readRange(float fromTime, float toTime);
    // The scanned lap that contains t; returns false if none (caller uses t / 0).
    bool currentLapAt(float t, float& startOut, int& numOut) const;

    // ── Load-time payload ────────────────────────────────────────────────────
    // { fastestLapNum, events, laps } — caller adds {"type":"playback_lap_blocks"}.
    nlohmann::json lapBlocksMessage() const;
    nlohmann::json getLapDataMessage(int lapNum) const;
private:
    struct IndexEntry { long offset; float sessionTime; uint8_t type; };
    struct LapBlock {
        int   lapNum;
        float startSessionTime;
        float endSessionTime;
        std::vector<nlohmann::json> telemetry;
        std::vector<nlohmann::json> statusHistory;
        std::vector<nlohmann::json> motionHistory;
        std::vector<nlohmann::json> motionExHistory;
        std::vector<nlohmann::json> damageHistory;
    };
    struct ScanLap { int lapNum; float startSessionTime; float endSessionTime; int lapTimeMs; };

    std::vector<IndexEntry> index_;
    std::FILE*  tempFile_   = nullptr;
    std::string tempPath_;
    long        tempFileSize_ = 0;
    float       startTime_  = 0.0f;
    float       totalTime_  = 0.0f;
    size_t      playPos_    = 0;

    // Scan products for playback mode.
    std::map<int, LapBlock>     lapBlocks_;
    std::vector<ScanLap>        scannedLaps_;
    std::vector<nlohmann::json> scannedEvents_;
    int                         fastestLapNum_ = 0;
    int                         fastestLapMs_  = 0;

    size_t upperBoundTime(float t) const;  // first index with sessionTime > t
    size_t lowerBoundTime(float t) const;  // first index with sessionTime >= t

    static bool decompress(const std::string& srcPath, const std::string& destPath);
    void buildIndex(const std::string& filePath);
    nlohmann::json readLineAt(long offset);
};

} // namespace tnrp
