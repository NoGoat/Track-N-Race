#pragma once

#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace tnrp {

// Reads a .tnrd file for playback. Decompresses to a temp file, builds a
// time/type index, and — in the same pass — builds the per-lap Speed/RPM/ERS
// comparison blocks, the scanned lap list and the event log that the renderer
// needs to enter true "playback mode".
//
// The hot streaming path (pullUntil / drainRest / stateSnapshot / readRange)
// returns raw JSONL strings — no re-parse needed. The load-time payload
// (lapBlocksMessage / getLapDataMessage) still returns nlohmann::json because
// those objects are built once at load time and passed on through Engine.
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

    // ── Playback streaming ───────────────────────────────────────────────────
    void setCursor(float t);
    std::vector<std::string> pullUntil(float t);
    std::vector<std::string> drainRest();
    bool hasMore() const { return playPos_ < index_.size(); }

    // ── Seek support ─────────────────────────────────────────────────────────
    std::vector<std::string> stateSnapshot(float t);
    std::vector<std::string> readRange(float fromTime, float toTime);
    bool currentLapAt(float t, float& startOut, int& numOut) const;

    // ── Load-time payload (nlohmann still used here — called once on load) ───
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
    std::FILE*  tempFile_    = nullptr;
    std::string tempPath_;
    long        tempFileSize_ = 0;
    float       startTime_   = 0.0f;
    float       totalTime_   = 0.0f;
    size_t      playPos_     = 0;

    std::map<int, LapBlock>     lapBlocks_;
    std::vector<ScanLap>        scannedLaps_;
    std::vector<nlohmann::json> scannedEvents_;
    int                         fastestLapNum_ = 0;
    int                         fastestLapMs_  = 0;

    size_t upperBoundTime(float t) const;
    size_t lowerBoundTime(float t) const;

    static bool decompress(const std::string& srcPath, const std::string& destPath);
    void buildIndex(const std::string& filePath);
    std::string readLine(long offset);   // reads raw JSONL line (no parse)
};

} // namespace tnrp
