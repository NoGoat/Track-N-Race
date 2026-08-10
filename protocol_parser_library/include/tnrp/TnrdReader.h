#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "tnrp/control_rows.h"
#include "tnrp/TnrdFormat.h"

namespace tnrp {

// Reads TNRD V1/gzip and TNRD V2/V3 Zstandard files for playback. load()
// detects the codec signature, then the JSON header distinguishes V2 from V3.
// Decompresses to a temp file, builds a
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
    bool loadZstd(const std::string& path, HeaderRow& outHeader);
    bool loadGzip(const std::string& path, HeaderRow& outHeader);
    void close();
    bool isLoaded() const { return tempFile_ != nullptr; }
    TnrdFormat loadedFormat() const { return loadedFormat_; }
    const std::string& lastError() const { return lastError_; }

    // Enable the Electron binary playback fast path BEFORE load(): the index
    // pass additionally pre-encodes the hot rows (telemetry/motion/motion_ex)
    // into a packed binary store (tnrp/BinaryRows.h), keeps the sparse cold
    // rows (status/damage/lap) for seek flushes, and fills the slim per-lap
    // chart points in lapBlocksMessage(). Off by default (JSON-only consumers).
    void setBinaryPlayback(bool on) { binaryPlayback_ = on; }

    // Include the small per-lap status summaries used for tyre labels without
    // building the binary playback stores.
    void setLapStatusSummaries(bool on) { lapStatusSummaries_ = on; }

    // Delete stale decompression temp files ("tracknrace_*.tmp") left in the OS
    // temp dir by earlier runs (or the Electron app) that exited abnormally, so
    // they can't accumulate and fill /tmp. Call once at startup — a running
    // session's own temp is created afterwards, so it is never a target. Files an
    // active instance still holds open are skipped (their unlink simply fails).
    // Best-effort and non-throwing: this runs during host startup, where a
    // malformed/unrepresentable entry in the shared OS temp directory must
    // never be allowed to take down either application.
    static void sweepStaleTempFiles() noexcept;

    float startTime() const { return startTime_; }
    float totalTime() const { return totalTime_; }

    // ── Playback streaming ───────────────────────────────────────────────────
    void setCursor(float t);
    std::vector<std::string> pullUntil(float t);
    std::vector<std::string> drainRest();
    bool hasMore() const { return playPos_ < index_.size(); }

    // Binary-playback variant of pullUntil: due cold rows are appended to
    // jsonOut as newline-terminated JSONL, due hot rows are appended to binOut
    // as packed records from the pre-built store, and seenTypes gets bit
    // (1 << tid) set for every due row's type id. If lastOfType is given, each
    // due cold row is also assigned to lastOfType[tid] (last one wins) so the
    // caller can maintain its sparse-row dup cache without re-scanning JSON.
    // Requires setBinaryPlayback before load. Pass +INFINITY to drain the rest.
    void pullUntilSplit(float t, std::string& jsonOut, std::vector<uint8_t>& binOut,
                        uint32_t& seenTypes,
                        std::array<std::string, 16>* lastOfType = nullptr);

    // ── Seek support ─────────────────────────────────────────────────────────
    std::vector<std::string> stateSnapshot(float t);
    // Latest row of each requested type at/before t (backward index walk; reads
    // only matched lines). Used to restore status/damage/positions panels on seek.
    std::vector<std::string> latestOfTypes(float t, const std::vector<uint8_t>& types);
    // Same walk as latestOfTypes but each line is returned tagged with its type
    // id, so the caller can key a per-type cache without re-scanning the JSON.
    std::vector<std::pair<uint8_t, std::string>> latestOfTypesTagged(
        float t, const std::vector<uint8_t>& types);
    std::vector<std::string> readRange(float fromTime, float toTime);
    // Reconstruct deduplicated Car Damage state at the UDP specification's
    // fixed 10 Hz cadence. Each returned row has session_time rewritten to its
    // sample tick and carries the latest recorded state at/before that tick.
    std::vector<std::string> damageRowsAtCadence(
        float fromTime, float toTime, bool includeFrom = true) const;
    bool currentLapAt(float t, float& startOut, int& numOut) const;

    // Binary seek flush (requires setBinaryPlayback before load): the hot rows
    // of [min(target-600, currentLapStart), target] as one packed binary slice,
    // plus the window's sparse cold rows (status/damage/lap) newline-joined.
    // allHistory expands the same packed payload to [startTime, target] for AL.
    struct SeekFlush { std::vector<uint8_t> binary; std::string coldJson; };
    SeekFlush seekFlush(float target, float currentLapStart, bool allHistory = false);

    // ── Load-time payload (built once on load, returned as serialised JSON) ──
    std::string lapBlocksMessage() const;             // full "playback_lap_blocks" row
    std::string getLapDataMessage(int lapNum) const;  // "playback_lap_data" row, "" if unknown lap

    // ── XLSX export (raw data dump, implemented in XlsxExport.cpp) ──────────
    // Walks the whole index in file order and writes one XLSX sheet per row
    // type encountered, plus an "Info" sheet from `header`. Requires isLoaded().
    // onProgress (if set) is called periodically from this same thread with
    // (rowsDone, totalUnits, stage) as the export proceeds — see XlsxExport.h.
    bool exportXlsx(const HeaderRow& header, const std::string& outPath,
                    std::string* errorOut = nullptr,
                    const std::function<void(size_t, size_t, const std::string&)>& onProgress = nullptr);

private:
    using FileOffset = std::int64_t;
    struct IndexEntry { FileOffset offset; float sessionTime; uint8_t type; };
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
        std::vector<LapProgressPoint> lapProgress; // populated only for V3
        std::vector<PlayerPositionPoint> playerPositions;
        // Slim chart points for lapBlocksMessage().
        std::vector<SlimTelemetryPoint> slimTelemetry;
        std::vector<SlimStatusPoint>    slimStatus;
    };
    struct ScanLap { int lapNum; float startSessionTime; float endSessionTime; int lapTimeMs; };

    std::vector<IndexEntry> index_;
    std::FILE*  tempFile_    = nullptr;
    std::string tempPath_;
    FileOffset  tempFileSize_ = 0;
    float       startTime_   = 0.0f;
    float       totalTime_   = 0.0f;
    size_t      playPos_     = 0;
    float       damageCadenceCursor_ = 0.0f;
    bool        binaryPlayback_ = false;
    bool        lapStatusSummaries_ = false;
    TnrdFormat  loadedFormat_ = TnrdFormat::Unknown;
    std::string lastError_;

    // Binary-playback stores (built by buildIndex when binaryPlayback_):
    // packed hot records + per-record times/byte-offsets (hotStart_ has one
    // sentinel end entry), a cumulative hot-record count per index position
    // (hotCum_[i] = hot records among index_[0..i), length index_.size()+1) so
    // an index range maps to a contiguous store slice, and the sparse cold
    // rows kept whole for seek flushes.
    std::vector<uint8_t>  hotBin_;
    std::vector<float>    hotTimes_;
    std::vector<size_t>   hotStart_;
    std::vector<uint32_t> hotCum_;
    std::vector<TimedRaw> coldStatus_;
    std::vector<TimedRaw> coldDamage_;
    std::vector<TimedRaw> coldLap_;
    std::vector<char>     scratch_;   // reused block-read buffer (pull/drain)

    std::map<int, LapBlock>  lapBlocks_;
    std::vector<ScanLap>     scannedLaps_;
    std::vector<std::string> scannedEvents_;   // raw race_event JSONL lines
    int                      fastestLapNum_ = 0;
    int                      fastestLapMs_  = 0;
    double                   initialFuelKg_ = -1.0;
    int                      trackLengthM_ = 0;

    size_t upperBoundTime(float t) const;
    size_t lowerBoundTime(float t) const;

    bool loadWithFormat(const std::string& path, HeaderRow& outHeader, TnrdFormat format);
    bool buildIndex(const std::string& filePath);
    std::string readLine(FileOffset offset);   // reads raw JSONL line (no parse)

    // Linear forward walk from playPos_ (same early-stop semantics as the old
    // per-row pullUntil on out-of-order rows): first index >= playPos_ whose
    // sessionTime exceeds t.
    size_t pullEnd(float t) const;
    // One contiguous fread of the lines behind index_[fromIdx..toIdx) into
    // scratch_; returns bytes read (0 on failure/empty range).
    size_t readBlock(size_t fromIdx, size_t toIdx);
};

} // namespace tnrp
