#pragma once

#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "tnrp/control_rows.h"

namespace tnrp {

// Reads a .tnrd file for playback. Decompresses to a temp file, builds a
// time/type index, and — in the same pass — builds the per-lap Speed/RPM/ERS
// comparison blocks, the scanned lap list and the event log that the renderer
// needs to enter true "playback mode".
//
// The hot streaming path (pullUntil / drainRest / stateSnapshot / readRange)
// returns raw JSONL strings — no re-parse needed. The load-time payload
// (lapBlocksMessage / getLapDataMessage) returns fully-serialised JSON strings,
// built once at load time with glaze (raw rows embedded verbatim via raw_json).
//
// Not thread-safe; the engine serializes access from its playback thread.
class TnrdReader {
public:
    TnrdReader() = default;
    ~TnrdReader();

    bool load(const std::string& path, HeaderRow& outHeader);
    void close();
    bool isLoaded() const { return tempFile_ != nullptr; }

    // Delete stale decompression temp files ("tracknrace_*.tmp") left in the OS
    // temp dir by earlier runs (or the Electron app) that exited abnormally, so
    // they can't accumulate and fill /tmp. Call once at startup — a running
    // session's own temp is created afterwards, so it is never a target. Files an
    // active instance still holds open are skipped (their unlink simply fails).
    static void sweepStaleTempFiles();

    float startTime() const { return startTime_; }
    float totalTime() const { return totalTime_; }

    // ── Playback streaming ───────────────────────────────────────────────────
    void setCursor(float t);
    std::vector<std::string> pullUntil(float t);
    std::vector<std::string> drainRest();
    bool hasMore() const { return playPos_ < index_.size(); }

    // ── Seek support ─────────────────────────────────────────────────────────
    std::vector<std::string> stateSnapshot(float t);
    // Latest row of each requested type at/before t (backward index walk; reads
    // only matched lines). Used to restore status/damage/positions panels on seek.
    std::vector<std::string> latestOfTypes(float t, const std::vector<uint8_t>& types);
    std::vector<std::string> readRange(float fromTime, float toTime);
    bool currentLapAt(float t, float& startOut, int& numOut) const;

    // ── Load-time payload (built once on load, returned as serialised JSON) ──
    std::string lapBlocksMessage() const;             // full "playback_lap_blocks" row
    std::string getLapDataMessage(int lapNum) const;  // "playback_lap_data" row, "" if unknown lap

    // ── XLSX export (raw data dump, implemented in XlsxExport.cpp) ──────────
    // Walks the whole index in file order and writes one XLSX sheet per row
    // type encountered, plus an "Info" sheet from `header`. Requires isLoaded().
    bool exportXlsx(const HeaderRow& header, const std::string& outPath,
                    std::string* errorOut = nullptr);

private:
    struct IndexEntry { long offset; float sessionTime; uint8_t type; };
    // A stored raw JSONL row plus its session_time (for ordering). The json is
    // emitted verbatim into the playback payload via glz::raw_json.
    struct TimedRaw { float t; std::string json; };
    struct LapBlock {
        int   lapNum;
        float startSessionTime;
        float endSessionTime;
        std::vector<TimedRaw> telemetry;
        std::vector<TimedRaw> statusHistory;
        std::vector<TimedRaw> motionHistory;
        std::vector<TimedRaw> motionExHistory;
        std::vector<TimedRaw> damageHistory;
    };
    struct ScanLap { int lapNum; float startSessionTime; float endSessionTime; int lapTimeMs; };

    std::vector<IndexEntry> index_;
    std::FILE*  tempFile_    = nullptr;
    std::string tempPath_;
    long        tempFileSize_ = 0;
    float       startTime_   = 0.0f;
    float       totalTime_   = 0.0f;
    size_t      playPos_     = 0;

    std::map<int, LapBlock>  lapBlocks_;
    std::vector<ScanLap>     scannedLaps_;
    std::vector<std::string> scannedEvents_;   // raw race_event JSONL lines
    int                      fastestLapNum_ = 0;
    int                      fastestLapMs_  = 0;

    size_t upperBoundTime(float t) const;
    size_t lowerBoundTime(float t) const;

    static bool decompress(const std::string& srcPath, const std::string& destPath);
    void buildIndex(const std::string& filePath);
    std::string readLine(long offset);   // reads raw JSONL line (no parse)
};

} // namespace tnrp
