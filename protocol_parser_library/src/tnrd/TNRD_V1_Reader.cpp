// TNRD_V1 playback reader. A V1 recording is one gzip stream of JSONL rows:
// it is decompressed to a temp file once at load, scanned into a time/type
// index, and read back by file offset.

#include "TNRD_V1.h"
#include "TnrdCodec.h"
#include "TnrdReaderBackend.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <optional>
#include <system_error>
#include <unordered_set>

#ifndef _WIN32
#include <sys/types.h>
#endif

#include "tnrp/BinaryRows.h"

// Partial reads of recorded rows. glaze's error_on_unknown_keys=false ignores
// every other key; missing keys keep their defaults. These must have external
// linkage (not an anonymous namespace): glaze's compile-time reflection takes
// the type's mangled name, which MSVC refuses for internal-linkage types
// (error C7631).
namespace tnrp::detail::tnrd_v1_reader {
struct LapScanFields {
    int lap_num{};
    int current_lap_ms{};
    int last_lap_ms{};
    float lap_distance_m{};
    int sector{};
    int s1_ms{};
    int s2_ms{};
};
// Partial read of a "status" row for Electron-only load-time chart metadata.
struct StatusScanFields {
    double ers_pct{};
    double fuel_kg{};
    int tyre_compound{};
    int visual_compound{};
};
struct SessionHistoryScanFields {
    std::optional<int> latest_lap_num;
    std::optional<int> latest_lap_time_ms;
};
struct ErsStatusFields {
    float session_time{};
    std::optional<float> engine_power_mguk_kw;
    std::optional<double> ers_j;
};
struct ErsLapPayload {
    std::vector<ErsStatusFields> statusHistory;
};
struct HarvestStatusFields {
    float session_time{};
    std::optional<double> ers_harvested_mguk_j;
    std::optional<double> ers_harvested_mguh_j;
};
struct HarvestLapPayload {
    std::vector<HarvestStatusFields> statusHistory;
};
struct InputTelemetryFields {
    float session_time{};
    std::optional<float> throttle;
    std::optional<float> brake;
    std::optional<float> speed_kph;
    std::optional<float> rpm;
    std::optional<int> gear;
    std::optional<float> steering;
};
struct InputLapPayload {
    std::vector<InputTelemetryFields> telemetry;
};
struct PositionLapPayload {
    std::vector<PlayerPositionPoint> playerPositions;
};
} // namespace tnrp::detail::tnrd_v1_reader

namespace tnrp::detail {
namespace {
using namespace tnrd_v1_reader;

constexpr glz::opts kPartialRead{ .null_terminated = false, .error_on_unknown_keys = false };

static float scanSessionTime(const char* d, int len) {
    static const char KEY[] = "\"session_time\":";
    static constexpr int KLEN = sizeof(KEY) - 1;
    for (int i = 0; i <= len - KLEN; ++i)
        if (d[i] == '"' && std::memcmp(d + i, KEY, KLEN) == 0)
            return std::strtof(d + i + KLEN, nullptr);
    return -1.0f;
}

static void setSessionTime(std::string& line, float t) {
    char num[32];
    std::snprintf(num, sizeof(num), "%.9g", (double)t);
    static const char KEY[] = "\"session_time\":";
    size_t key = line.find(KEY);
    if (key == std::string::npos) return;
    size_t valueStart = key + sizeof(KEY) - 1;
    size_t valueEnd = valueStart;
    while (valueEnd < line.size() &&
           line[valueEnd] != ',' && line[valueEnd] != '}') {
        ++valueEnd;
    }
    line.replace(valueStart, valueEnd - valueStart, num);
}

static uint8_t scanType(const char* d, int len) {
    static const char KEY[] = "\"type\":\"";
    static constexpr int KLEN = sizeof(KEY) - 1;
    for (int i = 0; i <= len - KLEN; ++i) {
        if (d[i] == '"' && std::memcmp(d + i, KEY, KLEN) == 0) {
            const char* v = d + i + KLEN;
            int r = len - (i + KLEN);
            if (r >= 9  && std::memcmp(v, "telemetry",   9)  == 0) return 1;
            if (r >= 6  && std::memcmp(v, "timing",       6)  == 0) return 7;
            if (r >= 4  && std::memcmp(v, "lap\"",        4)  == 0) return 4;
            if (r >= 6  && std::memcmp(v, "status",       6)  == 0) return 2;
            if (r >= 10 && std::memcmp(v, "all_status",  10)  == 0) return 9;
            if (r >= 12 && std::memcmp(v, "participants", 12) == 0) return 8;
            if (r >= 6  && std::memcmp(v, "damage",       6)  == 0) return 3;
            if (r >= 8  && std::memcmp(v, "session\"",     8)  == 0) return 5;
            if (r >= 10 && std::memcmp(v, "race_event",  10)  == 0) return 6;
            if (r >= 9  && std::memcmp(v, "tyre_sets",    9)  == 0) return 10;
            if (r >= 7  && std::memcmp(v, "motion\"",     7)  == 0) return 11;
            if (r >= 10 && std::memcmp(v, "motion_ex\"", 10)  == 0) return 12;
            if (r >= 9  && std::memcmp(v, "positions",    9)  == 0) return 13;
            if (r >= 23 && std::memcmp(v, "session_history_fastest", 23) == 0) return 14;
            return 0;
        }
    }
    return 0;
}


// The bit of a row type in a row-type mask.
constexpr uint32_t rowTypeBit(uint8_t type) { return type < 32 ? (1u << type) : 0; }

// std::fseek/std::ftell use a 32-bit long on Windows, even in a 64-bit build.
// TNRD recordings can decompress past 2 GiB, so every temp-file position must
// go through the platform's 64-bit stdio API.
std::int64_t tellFile(std::FILE* file) {
#ifdef _WIN32
    return static_cast<std::int64_t>(::_ftelli64(file));
#else
    return static_cast<std::int64_t>(::ftello(file));
#endif
}

bool seekFile(std::FILE* file, std::int64_t offset, int origin) {
    if (offset < 0) return false;
#ifdef _WIN32
    return ::_fseeki64(file, offset, origin) == 0;
#else
    if (offset > static_cast<std::int64_t>(std::numeric_limits<off_t>::max())) return false;
    return ::fseeko(file, static_cast<off_t>(offset), origin) == 0;
#endif
}

// Walks the lines of a readBlock()'d range in lockstep with its index entries.
// Lines the index pass skipped (length <= 1, see commitLine) are skipped here
// too without consuming an entry; a truncated unindexed tail past the last
// entry is never reached because the walk stops after `count` entries.
// `perEntry(entryIdx, lineData, lineLen)` is invoked once per index entry.
template <class F>
void walkBlockLines(const char* data, size_t got,
                           size_t fromIdx, size_t count, F&& perEntry) {
    const char* p   = data;
    const char* end = data + got;
    size_t consumed = 0;
    while (p < end && consumed < count) {
        const char* nl = static_cast<const char*>(std::memchr(p, '\n', end - p));
        const char* lineEnd = nl ? nl : end;
        int ll = (int)(lineEnd - p);
        if (ll > 1) {
            perEntry(fromIdx + consumed, p, ll);
            ++consumed;
        }
        if (!nl) break;
        p = nl + 1;
    }
}

class TnrdV1Reader final : public TnrdReaderBackend {
public:
    explicit TnrdV1Reader(TnrdReaderContext& context)
        : binaryPlayback_(context.binaryPlayback),
          sparseV6Playback_(context.sparseV6Playback),
          lapStatusSummaries_(context.lapStatusSummaries),
          playbackRowMask_(context.playbackRowMask),
          playbackOutputRowMask_(context.playbackOutputRowMask),
          strategyMinimumStops_(context.strategyMinimumStops),
          teamColorOverrides_(context.teamColorOverrides),
          playbackV6Types_(context.playbackV6Types),
          playbackV6HistoryTypes_(context.playbackV6HistoryTypes),
          lastError_(context.lastError) {}

    TnrdFormat format() const override { return loadedFormat_; }
    float startTime() const override { return startTime_; }
    float totalTime() const override { return totalTime_; }
    StrategySnapshotRow strategySnapshotAt(float t, StrategyProcessor* restoredProcessor = nullptr,
                                           const CancelCheck& cancelled = {}) override;
    void clearStrategyCheckpoints() override { strategyCheckpoints_.clear(); }
    std::string lapBlocksMessage() const override;

    // Nothing V6-only applies to this generation.
    void playbackV6TypesChanged(float) override {}
    void setPlaybackDriver(int, bool, float) override {}
    void setV6FocusDriver(int, float) override {}
    std::vector<std::string> v6FocusStatusSnapshot(float) override { return {}; }
    int effectivePlaybackDriver() const override { return -1; }
    bool wasRecoveredV6() const override { return false; }
    std::string driverRestrictionMessage(float) const override { return {}; }
    std::string phoneChartLapJson(int, const std::vector<std::string>&) const override { return {}; }
    std::shared_ptr<ErsPeakSweep> startErsPeakSweep() override { return {}; }
    std::shared_ptr<TrackCornerSweep> startTrackCornerSweep() override { return {}; }
    DriverLapHistoryRow driverLapHistory(int driver, float) const override {
        DriverLapHistoryRow out;
        out.car_idx = driver;
        return out;
    }
    LapMeasuresRow lapMeasures(int driver, float, bool) const override {
        LapMeasuresRow out;
        out.car_idx = driver;
        return out;
    }
    bool v6FastestLapAt(float, FastestLapRow&) const override { return false; }
    float v6NextLapEndAfter(float) const override { return std::numeric_limits<float>::infinity(); }
    // TNRD_V1 recordings have no column history.
    std::unique_ptr<TnrdReader::HistoryReader> openHistoryReader(std::string*) const override { return {}; }
    bool historyRequest(float, float, uint32_t, TnrdReader::HistoryRequest&) const override { return false; }

    // The lap's rows as getLapDataMessage() renders them, so Analysis reads
    // exactly what the Analysis charts read.
    std::vector<int> analysisLapNumbers(int driverIndex) const override;
    bool getAnalysisErsSamples(int lapNum, std::vector<ErsSample>& out, int driverIndex) const override;
    bool getAnalysisHarvestSamples(int lapNum, std::vector<HarvestSample>& out,
                                   int driverIndex) const override;
    bool getAnalysisInputSamples(int lapNum, std::vector<InputSample>& out,
                                 int driverIndex) const override;
    bool getAnalysisPositionSamples(int lapNum, std::vector<PlayerPositionPoint>& out,
                                    int driverIndex) const override;

    ~TnrdV1Reader() override;

    bool load(const std::string& path, HeaderRow& outHeader) override;
    bool isLoaded() const override { return tempFile_ != nullptr; }

    bool hasMore() const override { return playPos_ < index_.size(); }
    void setCursor(float t) override {
        playPos_ = upperBoundTime(t);
        damageCadenceCursor_ = t;
    }
    void beginCursorPrime(float t) override { setCursor(t); }
    void primeCursor() override {}
    void playbackRowMaskChanged(float) override {}
    std::vector<std::string> pullUntil(float t) override;
    void pullUntilSplit(float t, std::string& jsonOut, std::vector<uint8_t>& binOut,
                        uint32_t& seenTypes, std::array<std::string, 16>* lastOfType) override;

    std::vector<std::pair<uint8_t, std::string>> latestOfTypesTagged(
        float t, const std::vector<uint8_t>& types, const CancelCheck& cancelled) override;
    std::vector<std::string> readRange(float fromTime, float toTime) override;
    std::vector<std::string> damageRowsAtCadence(
        float fromTime, float toTime, bool includeFrom = true,
        const CancelCheck& cancelled = {}) const override;
    bool currentLapAt(float t, float& startOut, int& numOut) const override;
    SeekFlush seekFlush(float target, float currentLapStart, bool allHistory,
                        uint32_t requestedTypes, float windowSeconds,
                        bool includeMandatoryState, const CancelCheck& cancelled) override;

    std::string getLapDataMessage(int lapNum, uint32_t rowTypeMask, int driverIndex) const override;
    bool getAnalysisLapProgress(int lapNum, AnalysisLapProgress& out, int driverIndex) const override;
    std::vector<TrackStatusChange> aeroTrackStatusTimeline() const override { return {}; }

    bool exportParquet(const ParquetExportRequest& request, std::string* errorOut) override;

private:
    // A stored raw JSONL row plus its session_time (for ordering). The json is
    // emitted verbatim into the playback payload via glz::raw_json.
    struct TimedRaw { float t; std::string json; uint64_t sequence{}; };
    struct LapBlock {
        int   lapNum;
        float startSessionTime;
        float endSessionTime;
        std::vector<TimedRaw> telemetry;
        std::vector<TimedRaw> statusHistory;
        std::vector<TimedRaw> motionHistory;
        std::vector<TimedRaw> motionExHistory;
        std::vector<TimedRaw> damageHistory;
        std::vector<LapProgressPoint> lapProgress;
        std::vector<PlayerPositionPoint> playerPositions;
        // Slim chart points for lapBlocksMessage().
        std::vector<SlimTelemetryPoint> slimTelemetry;
        std::vector<SlimStatusPoint>    slimStatus;
        float sector1EndDistanceM{};
        float sector2EndDistanceM{};
    };
    struct ScanLap { int lapNum; float startSessionTime; float endSessionTime; int lapTimeMs; };

    // Strategy dependency rows in [fromTime, toTime], chronologically.
    bool forEachStrategyRow(float fromTime, float toTime, bool includeFrom,
                            const std::function<void(float, std::string_view)>& callback,
                            const CancelCheck& cancelled);

    // Host settings and the last error (TnrdReaderContext).
    const bool& binaryPlayback_;
    const bool& sparseV6Playback_;
    const bool& lapStatusSummaries_;
    uint32_t& playbackRowMask_;
    uint32_t& playbackOutputRowMask_;
    const int& strategyMinimumStops_;
    const TeamColorOverrides& teamColorOverrides_;
    const std::vector<uint8_t>& playbackV6Types_;
    const std::vector<uint8_t>& playbackV6HistoryTypes_;
    std::string& lastError_;

    static constexpr TnrdFormat loadedFormat_ = TnrdFormat::GzipV1;
    float                    startTime_   = 0.0f;
    float                    totalTime_   = 0.0f;
    float                    damageCadenceCursor_ = 0.0f;
    std::map<int, LapBlock>  lapBlocks_;
    std::vector<ScanLap>     scannedLaps_;
    std::vector<std::string> scannedEvents_;   // raw race_event JSONL lines
    std::vector<std::pair<float, StrategyProcessor>> strategyCheckpoints_;
    uint16_t                 strategyProtocol_ = 2025;
    int                      fastestLapNum_ = 0;
    int                      fastestLapMs_  = 0;
    double                   initialFuelKg_ = -1.0;
    int                      trackLengthM_ = 0;


    using FileOffset = std::int64_t;
    struct IndexEntry { FileOffset offset; float sessionTime; uint8_t type; };

    bool buildIndex(const std::string& filePath);
    std::string readLine(FileOffset offset);   // reads raw JSONL line (no parse)
    size_t upperBoundTime(float t) const;
    size_t lowerBoundTime(float t) const;
    // Linear forward walk from playPos_ (same early-stop semantics as the old
    // per-row pullUntil on out-of-order rows): first index >= playPos_ whose
    // sessionTime exceeds t.
    size_t pullEnd(float t) const;
    // One contiguous fread of the lines behind index_[fromIdx..toIdx) into
    // scratch_; returns bytes read (0 on failure/empty range).
    size_t readBlock(size_t fromIdx, size_t toIdx);

    std::vector<IndexEntry> index_;
    std::FILE*  tempFile_    = nullptr;
    std::string tempPath_;
    FileOffset  tempFileSize_ = 0;
    size_t      playPos_     = 0;

    // Binary-playback stores (built by buildIndex when binaryPlayback_):
    // packed hot records + per-record times/byte-offsets (hotStart_ has one
    // sentinel end entry), a cumulative hot-record count per index position
    // (hotCum_[i] = hot records among index_[0..i), length index_.size()+1) so
    // an index range maps to a contiguous store slice, and the sparse cold
    // rows kept whole for seek flushes.
    std::shared_ptr<std::vector<uint8_t>> hotBin_ = std::make_shared<std::vector<uint8_t>>();
    std::vector<float>    hotTimes_;
    std::vector<size_t>   hotStart_;
    std::vector<uint32_t> hotCum_;
    std::vector<TimedRaw> coldStatus_;
    std::vector<TimedRaw> coldDamage_;
    std::vector<TimedRaw> coldLap_;
    // The file is already scanned in full to build its index. Keep just the
    // cold strategy inputs so a seek never rereads/parses hot JSONL.
    std::vector<TimedRaw> legacyStrategyRows_;
    std::vector<char>     scratch_;   // reused block-read buffer (pull/drain)
};

TnrdV1Reader::~TnrdV1Reader() {
    if (tempFile_) { std::fclose(tempFile_); tempFile_ = nullptr; }
    std::fprintf(stderr, "[close-trace] temp FILE handle closed\n");
    std::fflush(stderr);
    if (!tempPath_.empty()) {
        std::error_code ec;
        std::filesystem::remove(tempPath_, ec);
        std::fprintf(stderr, "[close-trace] temp path remove returned error=%s\n",
                     ec ? ec.message().c_str() : "none");
        std::fflush(stderr);
    }
}

bool TnrdV1Reader::load(const std::string& path, HeaderRow& outHeader) {
    TNRD_V1::LoadResult loaded;
    if (!TNRD_V1::load(path, loaded, lastError_)) {
        std::fprintf(stderr, "[tnrd] load FAILED: %s for '%s'\n",
                     lastError_.c_str(), path.c_str());
        return false;
    }
    outHeader = std::move(loaded.header);
    tempPath_ = std::move(loaded.tempPath);
    trackLengthM_ = outHeader.track_length_m.value_or(0);
    if (loaded.partial)
        std::fprintf(stderr, "[tnrd] load: recovered a truncated %s stream; final partial row will be dropped\n",
                     toString(loadedFormat_));

    if (!buildIndex(tempPath_)) {
        lastError_ = "The decompressed recording could not be indexed.";
        std::fprintf(stderr, "[tnrd] load FAILED: 64-bit index read failed for '%s'\n", path.c_str());
        return false;
    }
    if (index_.empty()) {
        lastError_ = "The recording contains no readable telemetry rows.";
        std::fprintf(stderr, "[tnrd] load FAILED: index empty (no indexable rows) for '%s'\n", path.c_str());
        return false;
    }

    tempFile_ = openTnrdFile(tempPath_, "rb");
    if (!tempFile_) {
        lastError_ = "The decompressed recording could not be opened for playback.";
        std::fprintf(stderr, "[tnrd] load FAILED: final reopen of temp '%s' failed\n", tempPath_.c_str());
        return false;
    }
    if (!seekFile(tempFile_, 0, SEEK_END) || (tempFileSize_ = tellFile(tempFile_)) < 0) {
        lastError_ = "The decompressed recording size could not be read.";
        std::fprintf(stderr, "[tnrd] load FAILED: 64-bit size read failed for '%s'\n", path.c_str());
        return false;
    }
    strategyProtocol_ = static_cast<uint16_t>(outHeader.protocol >= 2024 ? outHeader.protocol : 2025);
    std::fprintf(stderr, "[tnrd] load OK: format=%s rows=%zu start=%.2f total=%.2f track='%s' session='%s'\n",
                 toString(loadedFormat_), index_.size(), startTime_, totalTime_,
                 outHeader.track_name.c_str(), outHeader.session_name.c_str());
    return true;
}

bool TnrdV1Reader::buildIndex(const std::string& filePath) {
    index_.clear();
    lapBlocks_.clear();
    scannedLaps_.clear();
    scannedEvents_.clear();
    hotBin_ = std::make_shared<std::vector<uint8_t>>();
    hotTimes_.clear();
    hotStart_.clear();
    hotCum_.clear();
    coldStatus_.clear();
    coldDamage_.clear();
    coldLap_.clear();
    legacyStrategyRows_.clear();
    fastestLapNum_ = 0;
    fastestLapMs_  = 0;
    initialFuelKg_ = -1.0;
    startTime_ = 0.0f;
    totalTime_ = 0.0f;
    playPos_   = 0;

    std::FILE* f = openTnrdFile(filePath, "rb");
    if (!f) {
        std::fprintf(stderr, "[tnrd] buildIndex: cannot open '%s'\n", filePath.c_str());
        return false;
    }
    // Reserve against vector regrowth during the scan: rows average well under
    // ~96 bytes of JSONL, so this slightly over-reserves and settles in one go.
    {
        const std::int64_t sz = seekFile(f, 0, SEEK_END) ? tellFile(f) : -1;
        if (sz < 0 || !seekFile(f, 0, SEEK_SET)) { std::fclose(f); return false; }
        if (sz > 0) index_.reserve((size_t)sz / 96);
    }
    { int c; while ((c = std::fgetc(f)) != EOF && c != '\n') {} }
    if (binaryPlayback_) hotCum_.push_back(0);

    constexpr size_t CHUNK = 2 * 1024 * 1024;
    std::vector<char> buf(CHUNK);
    std::string partial;
    std::int64_t partialOffset = tellFile(f);
    if (partialOffset < 0) { std::fclose(f); return false; }
    float lastT = 0.0f;
    bool  haveStart = false;

    int   curLapNum   = -1;
    float curLapStart = 0.0f;
    int   latestHistoryLapNum = 0;
    int   latestHistoryLapTimeMs = 0;

    auto commitLine = [&](const char* ld, int ll, std::int64_t lineOffset) {
        if (ll <= 1) return;
        float t = scanSessionTime(ld, ll);
        if (t < 0.0f) t = lastT;
        else          lastT = t;
        if (!haveStart) { startTime_ = t; haveStart = true; }
        uint8_t tid = scanType(ld, ll);
        index_.push_back({ lineOffset, t, tid });
        totalTime_ = std::max(totalTime_, t);
        if (kStrategyDependencyMask & (1u << tid))
            legacyStrategyRows_.push_back({t, std::string(ld, (size_t)ll)});

        // Binary playback: pre-encode the hot rows into the packed store (so a
        // playback tick / seek flush is a byte slice, not a re-serialisation)
        // and keep the sparse cold rows whole for seek flushes. hotCum_ tracks
        // the store record count per index position for range slicing.
        TelemetryRow telRow;   // parsed once for the hot store, reused for slim points
        StatusScanFields statusRow;
        if (tid == 3)
            coldDamage_.push_back({ t, std::string(ld, ll) });
        if (tid == 2 && (binaryPlayback_ || lapStatusSummaries_))
            (void)glz::read<kPartialRead>(statusRow, std::string_view(ld, (size_t)ll));
        if (binaryPlayback_) {
            std::string_view sv(ld, (size_t)ll);
            if (tid == 1) {
                (void)glz::read<kPartialRead>(telRow, sv);
                telRow.rev_lights_pct.reset();
                telRow.rev_lights_bit_value.reset();
                hotStart_.push_back(hotBin_->size());
                hotTimes_.push_back(t);
                bin::encodeTelemetry(*hotBin_, telRow);
            } else if (tid == 11) {
                MotionRow r;
                (void)glz::read<kPartialRead>(r, sv);
                hotStart_.push_back(hotBin_->size());
                hotTimes_.push_back(t);
                bin::encodeMotion(*hotBin_, r);
            } else if (tid == 12) {
                MotionExRow r;
                (void)glz::read<kPartialRead>(r, sv);
                hotStart_.push_back(hotBin_->size());
                hotTimes_.push_back(t);
                bin::encodeMotionEx(*hotBin_, r);
            } else if (tid == 2) {
                if (initialFuelKg_ < 0.0) initialFuelKg_ = statusRow.fuel_kg;
                coldStatus_.push_back({ t, std::string(ld, ll) });
            } else if (tid == 4) {
                coldLap_.push_back({ t, std::string(ld, ll) });
            }
            hotCum_.push_back((uint32_t)hotTimes_.size());
        }

        if (tid != 1 && tid != 2 && tid != 4 && tid != 6 && tid != 3 && tid != 11 && tid != 12 && tid != 13 && tid != 14) return;

        if (tid == 14) {
            SessionHistoryScanFields history;
            (void)glz::read<kPartialRead>(history, std::string_view(ld, (size_t)ll));
            if (history.latest_lap_num && history.latest_lap_time_ms &&
                *history.latest_lap_num > 0 && *history.latest_lap_time_ms > 0) {
                latestHistoryLapNum = *history.latest_lap_num;
                latestHistoryLapTimeMs = *history.latest_lap_time_ms;
            }
            return;
        }

        LapScanFields lf;
        if (tid == 4) {  // lap
            (void)glz::read<kPartialRead>(lf, std::string_view(ld, (size_t)ll));
            int lapNum = lf.lap_num;
            if (curLapNum < 0) {
                curLapNum   = lapNum;
                curLapStart = lf.current_lap_ms > 0 ? t - lf.current_lap_ms / 1000.0f : t;
            } else if (lapNum > curLapNum) {
                auto it = lapBlocks_.find(curLapNum);
                if (it != lapBlocks_.end()) {
                    it->second.endSessionTime = t;
                }
                int lapTimeMs = lf.last_lap_ms;
                scannedLaps_.push_back({ curLapNum, curLapStart, t, lapTimeMs });
                if (lapTimeMs > 0 && lapTimeMs < 300000 &&
                    (fastestLapMs_ == 0 || lapTimeMs < fastestLapMs_)) {
                    fastestLapMs_  = lapTimeMs;
                    fastestLapNum_ = curLapNum;
                }
                curLapNum   = lapNum;
                curLapStart = t;
            }
        }

        if (curLapNum >= 0) {
            auto it = lapBlocks_.find(curLapNum);
            if (it == lapBlocks_.end())
                it = lapBlocks_.emplace(curLapNum, LapBlock{ curLapNum, t, t }).first;
            it->second.endSessionTime = t;
            if (tid == 1)       it->second.telemetry.push_back({ t, std::string(ld, ll) });
            else if (tid == 2)  it->second.statusHistory.push_back({ t, std::string(ld, ll) });
            else if (tid == 3)  it->second.damageHistory.push_back({ t, std::string(ld, ll) });
            else if (tid == 11) it->second.motionHistory.push_back({ t, std::string(ld, ll) });
            else if (tid == 12) it->second.motionExHistory.push_back({ t, std::string(ld, ll) });
            else if (tid == 13) {
                PositionsRow positions;
                (void)glz::read<kPartialRead>(positions, std::string_view(ld, (size_t)ll));
                const auto player = std::find_if(
                    positions.cars.begin(), positions.cars.end(),
                    [&positions](const PositionCar& car) { return car.idx == positions.player_idx; });
                if (player != positions.cars.end())
                    it->second.playerPositions.push_back({ t, player->x, player->z });
            }

            if (binaryPlayback_ && tid == 1)
                it->second.slimTelemetry.push_back(
                    { "telemetry", t, telRow.speed_kph, telRow.rpm });
            else if ((binaryPlayback_ || lapStatusSummaries_) && tid == 2)
                it->second.slimStatus.push_back({
                    "status", t, statusRow.ers_pct,
                    statusRow.tyre_compound, statusRow.visual_compound
                });
        }

        if (tid == 6) {
            scannedEvents_.push_back(std::string(ld, ll));
        }
    };

    auto consumeChunk = [&](const char* base, size_t n, std::int64_t chunkStart) {
        const char* p = base;
        const char* end = base + n;
        while (p < end) {
            const char* nl = static_cast<const char*>(std::memchr(p, '\n', end - p));
            if (!nl) {
                size_t segLen = (size_t)(end - p);
                if (partial.empty()) partialOffset = chunkStart + (p - base);
                partial.append(p, segLen);
                break;
            }
            if (!partial.empty()) {
                partial.append(p, (size_t)(nl - p));
                commitLine(partial.data(), (int)partial.size(), partialOffset);
                partial.clear();
            } else {
                commitLine(p, (int)(nl - p), chunkStart + (p - base));
            }
            p = nl + 1;
        }
    };
    for (;;) {
        const std::int64_t chunkStart = tellFile(f);
        if (chunkStart < 0) { std::fclose(f); return false; }
        size_t n = std::fread(buf.data(), 1, CHUNK, f);
        if (n == 0) break;
        consumeChunk(buf.data(), n, chunkStart);
    }
    // A leftover partial here means the file ended without a trailing newline.
    // The writer always terminates every row (and the header) with '\n', so a
    // cleanly closed stream leaves nothing pending — a non-empty partial marks a
    // truncated tail (crash mid-write). Drop it rather than index a corrupt row.
    const bool readOk = std::ferror(f) == 0;
    std::fclose(f);
    if (!readOk) return false;

    if (curLapNum >= 0) {
        auto it = lapBlocks_.find(curLapNum);
        if (it != lapBlocks_.end()) it->second.endSessionTime = totalTime_;
        if (latestHistoryLapNum == curLapNum && latestHistoryLapTimeMs > 0) {
            scannedLaps_.push_back(
                { curLapNum, curLapStart, totalTime_, latestHistoryLapTimeMs });
            if (latestHistoryLapTimeMs < 300000 &&
                (fastestLapMs_ == 0 || latestHistoryLapTimeMs < fastestLapMs_)) {
                fastestLapMs_ = latestHistoryLapTimeMs;
                fastestLapNum_ = curLapNum;
            }
        }
    }
    auto byT = [](const TimedRaw& a, const TimedRaw& b) { return a.t < b.t; };
    std::stable_sort(legacyStrategyRows_.begin(), legacyStrategyRows_.end(), byT);
    std::sort(coldDamage_.begin(), coldDamage_.end(), byT);
    for (auto& kv : lapBlocks_) {
        std::sort(kv.second.telemetry.begin(), kv.second.telemetry.end(), byT);
        std::sort(kv.second.statusHistory.begin(), kv.second.statusHistory.end(), byT);
        std::sort(kv.second.motionHistory.begin(), kv.second.motionHistory.end(), byT);
        std::sort(kv.second.motionExHistory.begin(), kv.second.motionExHistory.end(), byT);
        std::sort(kv.second.damageHistory.begin(), kv.second.damageHistory.end(), byT);
        // Slim points sorted too — recordings can contain out-of-order UDP rows,
        // which would stall the renderer chart's advance cursor.
        std::sort(kv.second.slimTelemetry.begin(), kv.second.slimTelemetry.end(),
                  [](const SlimTelemetryPoint& a, const SlimTelemetryPoint& b) {
                      return a.session_time < b.session_time;
                  });
        std::sort(kv.second.slimStatus.begin(), kv.second.slimStatus.end(),
                  [](const SlimStatusPoint& a, const SlimStatusPoint& b) {
                      return a.session_time < b.session_time;
                  });
    }
    if (binaryPlayback_) hotStart_.push_back(hotBin_->size());   // sentinel end offset
    damageCadenceCursor_ = startTime_;
    return true;
}

size_t TnrdV1Reader::upperBoundTime(float t) const {
    size_t lo = 0, hi = index_.size();
    while (lo < hi) { size_t mid = (lo + hi) / 2; if (index_[mid].sessionTime <= t) lo = mid + 1; else hi = mid; }
    return lo;
}

size_t TnrdV1Reader::lowerBoundTime(float t) const {
    size_t lo = 0, hi = index_.size();
    while (lo < hi) { size_t mid = (lo + hi) / 2; if (index_[mid].sessionTime < t) lo = mid + 1; else hi = mid; }
    return lo;
}

// Returns the raw JSONL line at the given file offset (no JSON parse).
std::string TnrdV1Reader::readLine(FileOffset offset) {
    if (!tempFile_) return {};
    if (!seekFile(tempFile_, offset, SEEK_SET)) return {};
    std::string line;
    char buf[65536];
    while (std::fgets(buf, sizeof(buf), tempFile_)) {
        line += buf;
        if (!line.empty() && line.back() == '\n') {
            line.pop_back();  // strip trailing newline
            break;
        }
        if (std::strlen(buf) < sizeof(buf) - 1) break;
    }
    return line;
}

std::vector<std::string> TnrdV1Reader::damageRowsAtCadence(
    float fromTime, float toTime, bool includeFrom,
    const CancelCheck& cancelled) const {
    std::vector<std::string> out;
    if(cancelled&&cancelled())return out;
    const auto& damage = coldDamage_;
    if (damage.empty() || !std::isfinite(fromTime) ||
        !std::isfinite(toTime) || toTime < fromTime) {
        return out;
    }

    constexpr double RATE = 10.0;
    constexpr double EPS = 1e-6;
    const long long firstTick = includeFrom
        ? (long long)std::ceil((double)fromTime * RATE - EPS)
        : (long long)std::floor((double)fromTime * RATE + EPS) + 1;
    const long long lastTick =
        (long long)std::floor((double)toTime * RATE + EPS);
    if (firstTick > lastTick) return out;

    auto nextState = std::upper_bound(
        damage.begin(), damage.end(), (float)(firstTick / RATE),
        [](float t, const TimedRaw& row) { return t < row.t; });
    size_t stateIndex = nextState == damage.begin()
        ? damage.size()
        : (size_t)(nextState - damage.begin() - 1);

    out.reserve((size_t)(lastTick - firstTick + 1));
    for (long long tick = firstTick; tick <= lastTick; ++tick) {
        if((tick&255ll)==0&&cancelled&&cancelled())return {};
        const float sampleTime = (float)((double)tick / RATE);
        while (stateIndex != damage.size() &&
               stateIndex + 1 < damage.size() &&
               damage[stateIndex + 1].t <= sampleTime + (float)EPS) {
            ++stateIndex;
        }
        if (stateIndex == damage.size()) {
            if (damage.front().t > sampleTime + (float)EPS) continue;
            stateIndex = 0;
        }
        std::string row = damage[stateIndex].json;
        setSessionTime(row, sampleTime);
        out.push_back(std::move(row));
    }
    return out;
}

std::vector<std::pair<uint8_t, std::string>> TnrdV1Reader::latestOfTypesTagged(
    float t, const std::vector<uint8_t>& types, const CancelCheck& cancelled) {
    std::vector<std::pair<uint8_t, std::string>> out;
    if(cancelled&&cancelled())return out;
    if (index_.empty() || types.empty()) return out;
    size_t pos = upperBoundTime(t);
    std::unordered_set<uint8_t> wanted(types.begin(), types.end());
    std::vector<size_t> snapshot;
    for (size_t i = pos; i-- > 0 && !wanted.empty(); ) {
        if ((i & 255u) == 0 && cancelled && cancelled()) return {};
        if (wanted.erase(index_[i].type)) snapshot.push_back(i);
    }
    std::sort(snapshot.begin(), snapshot.end());
    for (size_t idx : snapshot) {
        std::string s = readLine(index_[idx].offset);
        if (!s.empty()) out.emplace_back(index_[idx].type, std::move(s));
    }
    return out;
}

std::vector<std::string> TnrdV1Reader::readRange(float fromTime, float toTime) {
    std::vector<std::string> out;
    if (!isLoaded() || index_.empty()) return out;
    size_t lo = lowerBoundTime(fromTime);
    size_t hi = upperBoundTime(toTime);
    if (lo >= hi) return out;

    const FileOffset startOff = index_[lo].offset;
    const FileOffset endOff   = (hi < index_.size()) ? index_[hi].offset : tempFileSize_;
    const FileOffset len = endOff - startOff;
    if (len <= 0) return out;
    if (static_cast<std::uint64_t>(len) > std::numeric_limits<size_t>::max()) return out;

    std::vector<char> buf((size_t)len);
    if (!seekFile(tempFile_, startOff, SEEK_SET)) return out;
    size_t got = std::fread(buf.data(), 1, (size_t)len, tempFile_);

    const char* p = buf.data();
    const char* end = p + got;
    while (p < end) {
        const char* nl = static_cast<const char*>(std::memchr(p, '\n', end - p));
        const char* lineEnd = nl ? nl : end;
        if (lineEnd > p) {
            int ll = (int)(lineEnd - p);
            float st = scanSessionTime(p, ll);
            if (st >= fromTime && st <= toTime)
                out.emplace_back(p, (size_t)ll);
        }
        if (!nl) break;
        p = nl + 1;
    }
    return out;
}

bool TnrdV1Reader::forEachStrategyRow(
    float fromTime, float toTime, bool includeFrom,
    const std::function<void(float, std::string_view)>& callback,
    const CancelCheck& cancelled) {
    if(cancelled&&cancelled())return false;
    if (toTime < fromTime) return true;
    if (!isLoaded()) return false;
    const auto begin = includeFrom
        ? std::lower_bound(legacyStrategyRows_.begin(), legacyStrategyRows_.end(), fromTime,
            [](const TimedRaw& row, float value) { return row.t < value; })
        : std::upper_bound(legacyStrategyRows_.begin(), legacyStrategyRows_.end(), fromTime,
            [](float value, const TimedRaw& row) { return value < row.t; });
    const auto end = std::upper_bound(begin, legacyStrategyRows_.end(), toTime,
        [](float value, const TimedRaw& row) { return value < row.t; });
    for (auto it = begin; it != end; ++it){if(cancelled&&cancelled())return false;callback(it->t, it->json);}
    return true;
}

bool TnrdV1Reader::currentLapAt(float t, float& startOut, int& numOut) const {
    // lapBlocks_ includes the final lap closed at EOF; scannedLaps_ only gains
    // an entry when the following lap begins, so it can never resolve the last
    // lap in a recording. At a shared boundary, prefer the block with the
    // latest start so seeking exactly to Lap N does not resolve to Lap N - 1.
    const LapBlock* match = nullptr;
    for (const auto& entry : lapBlocks_) {
        const LapBlock& block = entry.second;
        if (t >= block.startSessionTime && t <= block.endSessionTime &&
            (!match || block.startSessionTime > match->startSessionTime)) {
            match = &block;
        }
    }
    if (!match) return false;
    startOut = match->startSessionTime;
    numOut   = match->lapNum;
    return true;
}

std::string TnrdV1Reader::getLapDataMessage(int lapNum, uint32_t rowTypeMask,
                                           int driverIndex) const {
    (void)driverIndex;
    auto it = lapBlocks_.find(lapNum);
    if (it == lapBlocks_.end()) return {};
    const LapBlock& b = it->second;
    PlaybackLapDataRow msg;
    msg.lapNum           = b.lapNum;
    msg.startSessionTime = b.startSessionTime;
    msg.endSessionTime   = b.endSessionTime;
    msg.rowTypeMask      = rowTypeMask;
    auto fill = [](std::vector<glz::raw_json>& dst, const std::vector<TimedRaw>& src) {
        dst.reserve(src.size());
        for (const auto& e : src) dst.push_back(glz::raw_json{ e.json });
    };
    if(rowTypeMask&rowTypeBit(1))fill(msg.telemetry, b.telemetry);
    if(rowTypeMask&rowTypeBit(2))fill(msg.statusHistory, b.statusHistory);
    if(rowTypeMask&rowTypeBit(11))fill(msg.motionHistory, b.motionHistory);
    if(rowTypeMask&rowTypeBit(12))fill(msg.motionExHistory, b.motionExHistory);
    if(rowTypeMask&rowTypeBit(3))for(const auto&row:damageRowsAtCadence(b.startSessionTime,b.endSessionTime))msg.damageHistory.push_back(glz::raw_json{row});
    if(rowTypeMask&rowTypeBit(13))msg.playerPositions = b.playerPositions;
    return writeJson(msg);
}

bool TnrdV1Reader::getAnalysisLapProgress(int lapNum, AnalysisLapProgress& out,
                                         int driverIndex) const {
    (void)driverIndex;
    const auto it = lapBlocks_.find(lapNum);
    if (it == lapBlocks_.end()) return false;
    const LapBlock& block = it->second;

    AnalysisLapProgress result;
    result.lapNum = block.lapNum;
    result.startSessionTime = block.startSessionTime;
    result.endSessionTime = block.endSessionTime;
    result.trackLengthM = static_cast<float>(trackLengthM_);
    result.sector1EndDistanceM = block.sector1EndDistanceM;
    result.sector2EndDistanceM = block.sector2EndDistanceM;
    const auto completedLap = std::find_if(scannedLaps_.begin(), scannedLaps_.end(),
        [lapNum](const ScanLap& lap) { return lap.lapNum == lapNum; });
    if (completedLap != scannedLaps_.end()) result.lapTimeMs = completedLap->lapTimeMs;

    if (result.points.empty()) return false;
    // Lap Data carries completed S1/S2 durations on every later point. Keep the
    // last positive values so a synthetic finish point with zeroed split fields
    // cannot erase them.
    for (const auto& point : result.points) {
        if (point.s1_ms > 0) result.sector1TimeMs = point.s1_ms;
        if (point.s2_ms > 0) result.sector2TimeMs = point.s2_ms;
    }
    out = std::move(result);
    return true;
}

// First index >= playPos_ whose sessionTime exceeds t. Linear (not a binary
// search) on purpose: it matches the old per-row pullUntil's early stop at the
// first out-of-order row, so an ahead-of-time row is delivered when the cursor
// actually reaches it rather than skipped.
size_t TnrdV1Reader::pullEnd(float t) const {
    size_t end = playPos_;
    while (end < index_.size() && index_[end].sessionTime <= t) ++end;
    return end;
}

// One contiguous fread of the byte range behind index_[fromIdx..toIdx) into
// scratch_. Replaces the old per-row fseek+fgets walk — one seek + one read per
// tick instead of one per row, which dominated the playback tick cost.
size_t TnrdV1Reader::readBlock(size_t fromIdx, size_t toIdx) {
    if (!isLoaded() || fromIdx >= toIdx || toIdx > index_.size()) return 0;
    const FileOffset startOff = index_[fromIdx].offset;
    const FileOffset endOff   = (toIdx < index_.size()) ? index_[toIdx].offset : tempFileSize_;
    const FileOffset len = endOff - startOff;
    if (len <= 0) return 0;
    if (static_cast<std::uint64_t>(len) > std::numeric_limits<size_t>::max()) return 0;
    if (scratch_.size() < (size_t)len) scratch_.resize((size_t)len);
    if (!seekFile(tempFile_, startOff, SEEK_SET)) return 0;
    return std::fread(scratch_.data(), 1, (size_t)len, tempFile_);
}

std::vector<std::string> TnrdV1Reader::pullUntil(float t) {
    std::vector<std::string> out;
    size_t end = pullEnd(t);
    if (end == playPos_) return out;
    size_t got = readBlock(playPos_, end);
    if (got > 0) {
        out.reserve(end - playPos_);
        walkBlockLines(scratch_.data(), got, playPos_, end - playPos_,
                       [&](size_t, const char* ld, int ll) { out.emplace_back(ld, (size_t)ll); });
    }
    playPos_ = end;
    return out;
}

void TnrdV1Reader::pullUntilSplit(float t, std::string& jsonOut, std::vector<uint8_t>& binOut,
                                  uint32_t& seenTypes,
                                  std::array<std::string, 16>* lastOfType) {
    const float cadenceEnd = std::isfinite(t) ? t : totalTime_;
    size_t end = pullEnd(t);

    // Hot rows: the range's records are contiguous in the packed store, so the
    // whole tick's hot payload is a single byte-slice append.
    if (end > playPos_ && !hotCum_.empty()) {
        size_t hotLo = hotCum_[playPos_], hotHi = hotCum_[end];
        if (hotHi > hotLo) {
            const size_t byteBegin = hotStart_[hotLo];
            const size_t byteEnd = hotStart_[hotHi];
            (void)bin::appendFilteredBatch(binOut, hotBin_->data() + byteBegin,
                                           byteEnd - byteBegin, playbackRowMask_);
        }
    }

    // Cold rows: contiguous read, hot lines skipped (they went out as binary).
    size_t got = end > playPos_ ? readBlock(playPos_, end) : 0;
    if (got > 0) {
        walkBlockLines(scratch_.data(), got, playPos_, end - playPos_,
                       [&](size_t idx, const char* ld, int ll) {
            uint8_t tid = index_[idx].type;
            if (!(playbackRowMask_ & (1u << tid))) return;
            seenTypes |= (1u << tid);
            if (tid == 1 || tid == 11 || tid == 12) return;   // hot → binary path
            // Raw damage rows only advance the last-state cache. The emitted
            // chart stream below is exclusively the reconstructed 10 Hz series.
            if (tid == 3) {
                if (lastOfType) (*lastOfType)[tid].assign(ld, (size_t)ll);
                return;
            }
            jsonOut.append(ld, (size_t)ll);
            jsonOut.push_back('\n');
            if (lastOfType && tid < lastOfType->size()) (*lastOfType)[tid].assign(ld, (size_t)ll);
        });
    }
    playPos_ = end;

    if (playbackRowMask_ & (1u << 3)) {
        auto damageRows = damageRowsAtCadence(
            damageCadenceCursor_, cadenceEnd, false);
        for (auto& row : damageRows) {
            jsonOut += row;
            jsonOut.push_back('\n');
            seenTypes |= (1u << 3);
            if (lastOfType) (*lastOfType)[3] = row;
        }
    }
    damageCadenceCursor_ = cadenceEnd;
}

TnrdV1Reader::SeekFlush TnrdV1Reader::seekFlush(float target, float currentLapStart,
                                                bool allHistory, uint32_t requestedTypes,
                                                float windowSeconds, bool includeMandatoryState,
                                                const CancelCheck& cancelled) {
    (void)cancelled;
    SeekFlush f;
    // Comparison seeks restore the current lap, finite time-window seeks use
    // their exact session-time prefix, and AL asks for the full indexed prefix.
    // Every mode travels through the same packed flush path.
    const float windowStart = allHistory
        ? startTime_
        : windowSeconds > 0.0f
            ? std::max(startTime_, target - windowSeconds)
            : currentLapStart;
    const uint32_t mandatory = rowTypeBit(2) |
        rowTypeBit(3) | rowTypeBit(4);
    const uint32_t mask = (allHistory || !includeMandatoryState)
        ? requestedTypes : (requestedTypes | mandatory);

    if (!hotTimes_.empty()) {
        size_t lo = std::lower_bound(hotTimes_.begin(), hotTimes_.end(), windowStart) - hotTimes_.begin();
        size_t hi = std::upper_bound(hotTimes_.begin(), hotTimes_.end(), target) - hotTimes_.begin();
        if (hi > lo) {
            auto filtered = std::make_shared<std::vector<uint8_t>>();
            const size_t begin = hotStart_[lo], end = hotStart_[hi];
            filtered->reserve(end - begin);
            if (bin::appendFilteredBatch(*filtered, hotBin_->data() + begin,
                                         end - begin, mask) && !filtered->empty()) {
                f.binaryStore = std::move(filtered);
                f.binaryEnd = f.binaryStore->size();
            }
        }
    }

    // Cold rows: full linear scan (small, ~2 Hz), robust to the occasional
    // out-of-order row — exactly like the TS gatherCold.
    auto gather = [&](uint8_t rowType, const std::vector<TimedRaw>& rows) {
        if (!(mask & rowTypeBit(rowType))) return;
        for (const auto& r : rows) {
            if (r.t > target || r.t < windowStart) continue;
            f.coldJson += r.json;
            f.coldJson.push_back('\n');
        }
    };
    gather(2, coldStatus_);
    if (mask & rowTypeBit(3))
        for (auto& row : damageRowsAtCadence(windowStart, target)) {
            f.coldJson += row;
            f.coldJson.push_back('\n');
        }
    gather(4, coldLap_);
    if (!f.coldJson.empty()) f.coldJson.pop_back();   // no trailing newline
    return f;
}

StrategySnapshotRow TnrdV1Reader::strategySnapshotAt(float t, StrategyProcessor* restoredProcessor,
                                                     const CancelCheck& cancelled) {
    StrategyProcessor processor(strategyProtocol_);
    processor.setMinimumStops(strategyMinimumStops_);
    processor.setTeamColorOverrides(teamColorOverrides_);
    if(cancelled&&cancelled())return processor.snapshot();
    t = std::clamp(t, startTime_, totalTime_);
    float cursor = startTime_;
    bool restored = false;
    for (const auto& checkpoint : strategyCheckpoints_) {
        if (checkpoint.first > t) break;
        cursor = checkpoint.first;
        processor = checkpoint.second;
        processor.setMinimumStops(strategyMinimumStops_);
        restored = true;
    }

    // Decode the uncached prefix once, in chronological order, and take lap
    // checkpoints while walking it. Indexed formats select only the nine cold strategy row
    // families, so a late-race seek does not inflate telemetry/motion chunks.
    std::vector<float> boundaries;
    for (const auto& lap : scannedLaps_) {
        const float boundary = lap.endSessionTime;
        if (boundary <= cursor || boundary > t) continue;
        boundaries.push_back(boundary);
    }
    size_t nextBoundary = 0;
    auto checkpoint = [&] {
        const float boundary = boundaries[nextBoundary++];
        strategyCheckpoints_.emplace_back(boundary, processor);
        cursor = boundary;
    };

    bool replayOk = true;
    if (cursor < t || !restored)
        replayOk = forEachStrategyRow(cursor, t, !restored,
                                       [&](float rowTime, std::string_view json) {
            // Rows stamped exactly at a boundary belong to the completed lap.
            // Finalize only when the first later row is encountered.
            while (nextBoundary < boundaries.size() && boundaries[nextBoundary] < rowTime)
                checkpoint();
            processor.ingestJson(json);
        },cancelled);
    if (replayOk)
        while (nextBoundary < boundaries.size()) checkpoint();
    StrategySnapshotRow result = processor.snapshot();
    if (restoredProcessor) *restoredProcessor = std::move(processor);
    return result;
}

std::string TnrdV1Reader::lapBlocksMessage() const {
    PlaybackLapBlocksRow msg;
    for (const auto& kv : lapBlocks_) {
        const LapBlock& b = kv.second;
        // Slim vectors are empty unless binary playback built them; the copy is
        // what lets this method stay const and the message one-shot at load.
        msg.blocks.push_back({ b.lapNum, b.startSessionTime, b.endSessionTime,
                               b.slimTelemetry, b.slimStatus,
                               b.sector1EndDistanceM, b.sector2EndDistanceM });
    }
    msg.fastestLapNum = fastestLapNum_;
    msg.initialFuelKg = initialFuelKg_;
    msg.tnrdVersion = "TNRD_V1";
    msg.deltaAvailable = false;
    msg.lapDistanceAvailable = false;
    msg.trackLengthM = 0;
    msg.events.reserve(scannedEvents_.size());
    for (const auto& s : scannedEvents_) msg.events.push_back(glz::raw_json{ s });
    for (const auto& l : scannedLaps_)   msg.laps.push_back({ l.lapNum, l.lapTimeMs });
    return writeJson(msg);
}

std::vector<int> TnrdV1Reader::analysisLapNumbers(int driverIndex) const {
    (void)driverIndex;
    std::vector<int> laps;
    for (const auto& [lapNum, block] : lapBlocks_) laps.push_back(lapNum);
    return laps;
}

bool TnrdV1Reader::getAnalysisErsSamples(int lapNum, std::vector<ErsSample>& out,
                                    int driverIndex) const {
    constexpr uint32_t statusRows = 1u << 2;
    const std::string json = getLapDataMessage(lapNum, statusRows, driverIndex);
    if (json.empty()) return false;
    ErsLapPayload payload;
    if (glz::read<kPartialRead>(payload, json)) return false;
    out.clear();
    out.reserve(payload.statusHistory.size());
    for (const auto& row : payload.statusHistory) {
        if (!row.engine_power_mguk_kw || !std::isfinite(*row.engine_power_mguk_kw)) continue;
        out.push_back({row.session_time, *row.engine_power_mguk_kw, row.ers_j.value_or(0.0)});
    }
    return true;
}

bool TnrdV1Reader::getAnalysisHarvestSamples(int lapNum, std::vector<HarvestSample>& out,
                                        int driverIndex) const {
    constexpr uint32_t statusRows = 1u << 2;
    const std::string json = getLapDataMessage(lapNum, statusRows, driverIndex);
    if (json.empty()) return false;
    HarvestLapPayload payload;
    if (glz::read<kPartialRead>(payload, json)) return false;
    out.clear();
    out.reserve(payload.statusHistory.size());
    for (const auto& row : payload.statusHistory)
        if (row.ers_harvested_mguk_j || row.ers_harvested_mguh_j)
            out.push_back({row.session_time,
                           row.ers_harvested_mguk_j.value_or(0.0) + row.ers_harvested_mguh_j.value_or(0.0)});
    return true;
}

bool TnrdV1Reader::getAnalysisInputSamples(int lapNum, std::vector<InputSample>& out,
                                      int driverIndex) const {
    constexpr uint32_t telemetryRows = 1u << 1;
    const std::string json = getLapDataMessage(lapNum, telemetryRows, driverIndex);
    if (json.empty()) return false;
    InputLapPayload payload;
    if (glz::read<kPartialRead>(payload, json)) return false;
    out.clear();
    out.reserve(payload.telemetry.size());
    for (const auto& row : payload.telemetry)
        if (row.throttle || row.brake)
            out.push_back({row.session_time, row.throttle.value_or(0.0f), row.brake.value_or(0.0f),
                           row.speed_kph.value_or(0.0f), row.rpm.value_or(0.0f), row.gear.value_or(0),
                           row.steering.value_or(0.0f)});
    return true;
}

bool TnrdV1Reader::getAnalysisPositionSamples(int lapNum, std::vector<PlayerPositionPoint>& out,
                                         int driverIndex) const {
    constexpr uint32_t positionRows = 1u << 13;
    const std::string json = getLapDataMessage(lapNum, positionRows, driverIndex);
    if (json.empty()) return false;
    PositionLapPayload payload;
    if (glz::read<kPartialRead>(payload, json)) return false;
    out = std::move(payload.playerPositions);
    return true;
}


// Hands every indexed row to the V1 exporter in file order, reading the
// temp file in contiguous blocks (one seek per block, as playback does).
bool TnrdV1Reader::exportParquet(const ParquetExportRequest& request, std::string* errorOut) {
    if (!isLoaded()) {
        if (errorOut) *errorOut = "no .tnrd loaded";
        return false;
    }
    JsonlExportSource source;
    source.rowCount = index_.size();
    source.forEach = [this](const JsonlExportSource::Visitor& visit, std::string* readError) {
        constexpr size_t kBlockRows = 8192;
        for (size_t from = 0; from < index_.size(); from += kBlockRows) {
            const size_t to = std::min(index_.size(), from + kBlockRows);
            const size_t got = readBlock(from, to);
            if (got == 0) {
                if (readError) *readError = "could not read the decompressed recording";
                return false;
            }
            size_t visited = 0;
            bool keepGoing = true;
            walkBlockLines(scratch_.data(), got, from, to - from, [&](size_t i, const char* line, int length) {
                ++visited;
                if (keepGoing) keepGoing = visit(index_[i].sessionTime, std::string_view(line, (size_t)length));
            });
            if (!keepGoing) return false;
            if (visited != to - from) {
                if (readError) *readError = "the decompressed recording is shorter than its index";
                return false;
            }
        }
        return true;
    };
    return exportTnrdV1Parquet(source, request, errorOut);
}

} // namespace

std::unique_ptr<TnrdReaderBackend> makeTnrdV1Reader(TnrdReaderContext& context) {
    return std::make_unique<TnrdV1Reader>(context);
}

} // namespace tnrp::detail
