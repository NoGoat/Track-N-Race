#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <optional>
#include <vector>

#include "tnrp/control_rows.h"
#include "tnrp/AnalysisMap.h"
#include "tnrp/LapDelta.h"
#include "tnrp/LapMeasures.h"
#include "tnrp/Strategy.h"
#include "tnrp/TnrdFormat.h"

namespace tnrp {

namespace detail {
class TnrdV6Archive;
class TnrdReaderBackend;

// What a reader keeps across recordings: the host's playback settings and the
// last error. Each loaded recording's backend reads and updates it in place.
struct TnrdReaderContext {
    bool binaryPlayback = false;
    bool sparseV6Playback = false;
    bool lapStatusSummaries = false;
    uint32_t playbackRowMask = 0xFFFFFFFFu;
    uint32_t playbackOutputRowMask = 0xFFFFFFFFu;
    int strategyMinimumStops = 0;
    TeamColorOverrides teamColorOverrides;
    std::vector<uint8_t> playbackV6Types;
    std::vector<uint8_t> playbackV6HistoryTypes;
    std::string lastError;
};
} // namespace detail

// Reads every TNRD generation. load() detects the container signature (the
// legacy JSON header distinguishes V2 from V3) and hands the recording to that
// generation's reader backend, which lives in its own module under src/tnrd/
// (TNRD_V1_Reader.cpp ... TNRD_V6_Reader.cpp). This class keeps only what is
// independent of the format: host settings, the last error and analysis caches.
//
// The hot streaming path (pullUntil / drainRest / stateSnapshot / readRange)
// returns raw JSONL strings — no re-parse needed. The load-time payload
// (lapBlocksMessage / getLapDataMessage) returns fully-serialised JSON strings.
//
// Not thread-safe; the engine serializes access from its playback thread.
class TnrdReader {
public:
    TnrdReader();
    ~TnrdReader();

    // What a file list shows of a recording: its header, its length and how
    // many drivers it holds (0 when the format does not say). Read from the
    // index of a V4–V6 file without loading it; false for V1–V3, which would
    // have to be decompressed, and for anything unreadable.
    struct Summary {
        HeaderRow header;
        float durationSeconds = 0.0f;
        int drivers = 0;
    };
    static bool summarize(const std::string& path, Summary& out, std::string* errorOut = nullptr);

    bool load(const std::string& path, HeaderRow& outHeader);
    bool loadZstd(const std::string& path, HeaderRow& outHeader);
    bool loadGzip(const std::string& path, HeaderRow& outHeader);
    // Adopts a V6 archive that is already open, such as one over a live
    // session's memory image (TnrdV6Archive::openMemory), for Strategy
    // reconstruction only: no playback cursor or sector metadata is prepared,
    // so strategySnapshotAt() is the one meaningful call afterwards.
    bool loadV6ArchiveForStrategy(std::unique_ptr<detail::TnrdV6Archive> archive,
                                  const HeaderRow& header);
    void close();
    bool isLoaded() const;
    TnrdFormat loadedFormat() const { return loadedFormat_; }
    // True when a V6 recording was opened by rebuilding its index from the
    // chunk stream because the writer was interrupted. The final lap of each
    // driver may be absent; callers should say so rather than stay silent.
    bool wasRecoveredV6() const;
    const std::string& lastError() const { return context_.lastError; }

    // Enable packed binary delivery for hot playback rows and seek flushes.
    // Indexed playback decodes and packs requested chunks on demand. V5 keeps
    // its existing lazy-load behavior; V4 also avoids a full-session hot-row
    // mirror and retains only its load-time strategy checkpoints.
    void setBinaryPlayback(bool on) { context_.binaryPlayback = on; }
    void setSparseV6Playback(bool on) { context_.sparseV6Playback = on; }

    // Include the small per-lap status summaries used for tyre labels without
    // building the binary playback stores.
    void setLapStatusSummaries(bool on) { context_.lapStatusSummaries = on; }

    // Delete stale decompression temp files ("tracknrace_*.tmp") left in the OS
    // temp dir by earlier runs (or the Electron app) that exited abnormally, so
    // they can't accumulate and fill /tmp. Call once at startup — a running
    // session's own temp is created afterwards, so it is never a target. Files an
    // active instance still holds open are skipped (their unlink simply fails).
    // Best-effort and non-throwing: this runs during host startup, where a
    // malformed/unrepresentable entry in the shared OS temp directory must
    // never be allowed to take down either application.
    static void sweepStaleTempFiles() noexcept;

    float startTime() const;
    float totalTime() const;

    // ── Playback streaming ───────────────────────────────────────────────────
    // Changes the logical row families loaded/emitted by sequential playback.
    // Indexed V4/V5 reload only the current lap's selected chunks at the supplied cursor;
    // legacy formats retain their index and filter the packed output.
    void setPlaybackRowMask(uint32_t mask, float cursorTime);
    void setPlaybackV6Types(const std::vector<uint8_t>& streamTypes,
                            const std::vector<uint8_t>& historyTypes,
                            float cursorTime);
    // What setPlaybackV6Types last installed; empty (also after close()) means
    // every field.
    const std::vector<uint8_t>& playbackV6Types() const { return context_.playbackV6Types; }
    const std::vector<uint8_t>& playbackV6HistoryTypes() const { return context_.playbackV6HistoryTypes; }
    // The V6 driver whose data playback currently follows.
    int effectivePlaybackDriver() const;
    // V6 only: project driver-scoped rows for exactly the selected driver.
    // The recorded driver keeps the original private/player rows, but the V6
    // all-car payload attached to those rows is stripped before emission.
    void setPlaybackDriver(int driverIndex, bool useRecordedRows, float cursorTime);
    // V6 only: the Standings-selected car whose private status (ERS, fuel,
    // aero, brake bias) is read alongside the playback driver's. Every other
    // car contributes only its lap timing and tyre state. -1 clears it.
    void setV6FocusDriver(int driverIndex, float cursorTime);
    // That car's private status at `t`, as all_status patches.
    std::vector<std::string> v6FocusStatusSnapshot(float t);
    void setCursor(float t);
    // Position the indexed playback lanes and start loading the V5 chunks that
    // contain/follow the target. This is deliberately separate from
    // primeCursor(): seek backfill extraction can run while these low-priority
    // reads are in flight, and the archive's in-flight table deduplicates any
    // chunk needed by both operations.
    void beginCursorPrime(float t);
    // Prepare the indexed chunk frontier at the current cursor without
    // emitting rows. Engine seek calls this before releasing its seek gate.
    void primeCursor();
    std::vector<std::string> pullUntil(float t);
    std::vector<std::string> drainRest();
    bool hasMore() const;

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
    std::vector<std::string> stateSnapshot(float t,
                                           const std::function<bool()>& cancelled = {});
    // Latest row of each requested type at/before t (backward index walk; reads
    // only matched lines). Used to restore status/damage/positions panels on seek.
    std::vector<std::string> latestOfTypes(float t, const std::vector<uint8_t>& types,
                                           const std::function<bool()>& cancelled = {});
    // Same walk as latestOfTypes but each line is returned tagged with its type
    // id, so the caller can key a per-type cache without re-scanning the JSON.
    std::vector<std::pair<uint8_t, std::string>> latestOfTypesTagged(
        float t, const std::vector<uint8_t>& types,
        const std::function<bool()>& cancelled = {});
    std::vector<std::string> readRange(float fromTime, float toTime);
    // Derived strategy seek state. Replays only the strategy dependency rows in
    // chronological order, materializing reusable checkpoints at completed lap
    // boundaries. Checkpoints are never written to the recording.
    StrategySnapshotRow strategySnapshotAt(float t, StrategyProcessor* restoredProcessor = nullptr,
                                             const std::function<bool()>& cancelled = {});
    void setStrategyMinimumStops(int stops);
    void setTeamColorOverrides(TeamColorOverrides overrides);
    // Reconstruct deduplicated Car Damage state at the UDP specification's
    // fixed 10 Hz cadence. Each returned row has session_time rewritten to its
    // sample tick and carries the latest recorded state at/before that tick.
    std::vector<std::string> damageRowsAtCadence(
        float fromTime, float toTime, bool includeFrom = true,
        const std::function<bool()>& cancelled = {}) const;
    bool currentLapAt(float t, float& startOut, int& numOut) const;

    // Binary seek flush (requires setBinaryPlayback before load): the hot rows
    // leading up to target as one packed binary slice, plus the same range's
    // sparse cold rows (status/damage/lap) newline-joined. allHistory starts at
    // the session start; a positive windowSeconds starts at target-windowSeconds;
    // otherwise comparison/current-lap mode starts at currentLapStart.
    struct SeekFlush {
        std::shared_ptr<const std::vector<uint8_t>> binaryStore;
        size_t binaryBegin = 0;
        size_t binaryEnd = 0;
        std::string coldJson;
    };
    SeekFlush seekFlush(float target, float currentLapStart, bool allHistory = false,
                        uint32_t requestedTypes = 0xFFFFFFFFu,
                        float windowSeconds = 0.0f,
                        bool includeMandatoryState = true,
                        const std::function<bool()>& cancelled = {});

    // ── Chart history on a second handle ─────────────────────────────────────
    // What one history read asks for; historyRequest() fills it.
    struct HistoryRequest {
        uint8_t driver = 0;
        std::vector<uint8_t> types;   // empty = every field
        uint32_t mask = 0;
        float from = 0.0f;
        float to = 0.0f;
        bool seed = false;            // carry each field's value at `from`
    };
    // An independent read-only handle on the loaded recording, so seek and
    // All Laps history can be read while playback keeps using this reader.
    // Takes one read at a time.
    class HistoryReader {
    public:
        virtual ~HistoryReader() = default;
        // The history as binary column blocks (the layout Electron's
        // lib/columnStore.ts decodes).
        virtual bool read(const HistoryRequest& request, std::vector<uint8_t>& out,
                          std::string* errorOut, const std::function<bool()>& cancelled) = 0;
    };
    // Null when this recording has no column history (only V6 recordings
    // opened from a file do), leaving errorOut alone, or when the handle
    // could not open, with errorOut saying why.
    std::unique_ptr<HistoryReader> openHistoryReader(std::string* errorOut = nullptr) const;
    // The read for [from, to] of the playback driver's history; false when
    // this recording or the playback settings cannot serve it column-wise.
    bool historyRequest(float from, float to, uint32_t mask, HistoryRequest& out) const;

    // ── Load-time payload (built once on load, returned as serialised JSON) ──
    std::string lapBlocksMessage() const;             // full "playback_lap_blocks" row
    // "driver_restriction" for the selected driver at cursorTime, or "" when
    // this is not a V6 recording (no other format stores the setting).
    std::string driverRestrictionMessage(float cursorTime) const;
    std::string getLapDataMessage(int lapNum, uint32_t rowTypeMask = 0xFFFFFFFFu,
                                  int driverIndex = -1) const;
    // A V6 recording's lap `lapNum` for the playback driver as a phone's chart
    // lap reply (PairLapData's format, without positions), read straight from
    // the columns rather than rendered as a playback_lap_data row and parsed
    // back. Empty when the column path does not cover this recording or
    // driver selection; the caller then uses getLapDataMessage.
    std::string phoneChartLapJson(int lapNum, const std::vector<std::string>& channels) const;
    bool getAnalysisLapProgress(int lapNum, AnalysisLapProgress& out,
                                int driverIndex = -1) const;
    // ── Analysis map inputs (tnrp/AnalysisMap.h) ──
    // The laps getAnalysisLapProgress can answer for this driver.
    std::vector<int> analysisLapNumbers(int driverIndex = -1) const;
    // One lap's Car Status MGU-K and ERS store samples for this driver.
    bool getAnalysisErsSamples(int lapNum, std::vector<ErsSample>& out,
                               int driverIndex = -1) const;
    // One lap's MGU-K + MGU-H harvest counter samples for this driver.
    bool getAnalysisHarvestSamples(int lapNum, std::vector<HarvestSample>& out,
                                   int driverIndex = -1) const;
    // One lap's throttle and brake samples for this driver.
    bool getAnalysisInputSamples(int lapNum, std::vector<InputSample>& out,
                                 int driverIndex = -1) const;
    // The race's active-aero track status over time, from its Session rows.
    // Empty when the recording has none (formats before 2026, V1-V3 files).
    std::vector<TrackStatusChange> aeroTrackStatusTimeline() const;
    // A car's whole-recording ERS peaks, kept once scanned. loadSerial()
    // changes with every load/close, so a scan that finishes after another
    // recording was opened is not stored against it.
    uint64_t loadSerial() const { return loadSerial_; }
    bool cachedErsPeaks(int driverIndex, ErsPeaks& out) const;
    void cacheErsPeaks(int driverIndex, uint64_t serial, const ErsPeaks& peaks) const;
    // The car a driver index names: -1 is the car playback follows.
    int analysisCarIndex(int driverIndex) const {
        return driverIndex >= 0 ? driverIndex : effectivePlaybackDriver();
    }
    // V6 recordings opened from a file: finds every car's ERS peaks on
    // background threads, each reading its own copy of the archive, so the
    // first ERS Usage map does not wait for a scan. Hosts call it after
    // load() for recordings they will analyse; close() cancels it.
    void startErsPeakSweep();
    std::shared_ptr<ErsPeakSweep> ersPeakSweep() const { return ersPeakSweep_; }
    // V6 recordings opened from a file: finds the track's corners from every
    // car's laps on background threads, each reading its own copy of the
    // archive, for the Speed and braking maps to place corners by. Hosts call
    // it after load() for recordings they will analyse; close() cancels it.
    void startTrackCornerSweep();
    std::shared_ptr<TrackCornerSweep> trackCornerSweep() const { return trackCornerSweep_; }
    // One lap's world positions for this driver, in time order.
    bool getAnalysisPositionSamples(int lapNum, std::vector<PlayerPositionPoint>& out,
                                    int driverIndex = -1) const;
    // When this driver was penalised or warned for track limits (corner
    // cutting or running wide), from the recording's penalty events, in the
    // recording's order.
    std::vector<float> trackLimitTimes(int driverIndex = -1) const;
    // Track corners found without a sweep, from the playback car's own laps,
    // kept once found (an empty list when there were none). `serial` is
    // loadSerial() when the scan began.
    bool cachedTrackCorners(TrackCorners& out) const;
    void cacheTrackCorners(uint64_t serial, const TrackCorners& corners) const;
    // One car's completed laps ending at or before `throughTime`. V6 only;
    // other formats return no laps.
    DriverLapHistoryRow driverLapHistory(int driver, float throughTime) const;
    // That car's ERS and fuel use on each of the same laps (tnrp/LapMeasures.h).
    // V6 only; other formats return no laps.
    LapMeasuresRow lapMeasures(int driver, float throughTime, bool hasMguh) const;
    // V6 recordings keep no fastest_lap rows. The holder at `t` is derived
    // from the lap summaries: the fastest valid completed lap by then. Returns
    // false for other formats, whose recorded rows already carry it; car_idx
    // is -1 while nobody has one yet.
    bool v6FastestLapAt(float t, FastestLapRow& out) const;
    // The first completed-lap end strictly after `t`, across every car: the
    // next moment a lap-times view can change. +inf when there is none or
    // the recording is not V6.
    float v6NextLapEndAfter(float t) const;

    // ── Parquet export (implemented in ParquetExport.cpp) ───────────────────
    // Writes every stored row as Parquet tables, one <Table>.parquet each, into
    // `directory` (an existing folder, UTF-8) through this generation's
    // exporter; `header` is recorded in each file's metadata. Requires
    // isLoaded(). onProgress gets (fraction 0..1, stage) on this thread;
    // `cancelled` is polled between rows. tnrp/ParquetExport.h wraps this
    // with a staged, never-overwriting output folder.
    bool exportParquet(const HeaderRow& header, const std::string& directory,
                       std::string* errorOut = nullptr,
                       const std::function<void(double, const std::string&)>& onProgress = nullptr,
                       const std::function<bool()>& cancelled = nullptr);

private:
    bool loadWithFormat(const std::string& path, HeaderRow& outHeader, TnrdFormat format);

    detail::TnrdReaderContext context_;
    std::unique_ptr<detail::TnrdReaderBackend> backend_;
    TnrdFormat               loadedFormat_ = TnrdFormat::Unknown;
    uint64_t                 loadSerial_ = 0;
    std::shared_ptr<ErsPeakSweep> ersPeakSweep_;
    std::shared_ptr<TrackCornerSweep> trackCornerSweep_;
    mutable std::optional<TrackCorners> trackCorners_;
    mutable std::map<int, ErsPeaks> ersPeaks_;
};

} // namespace tnrp
