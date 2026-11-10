// TNRD_V4 playback reader. A V4 recording is indexed: a directory of laps and
// row-family chunks, each chunk an independent Zstandard frame of JSONL rows
// that is decompressed on demand.
// V4's directory has no exact chunk time bounds: playback discovers them by
// decoding, and strategy checkpoints are taken in one pass at load.

#include "TNRD_V4.h"
#include "TnrdCodec.h"
#include "TnrdReaderBackend.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <list>
#include <map>
#include <optional>
#include <thread>
#include <unordered_map>

#include "tnrp/BinaryRows.h"

// Partial reads of recorded rows. glaze's error_on_unknown_keys=false ignores
// every other key; missing keys keep their defaults. These must have external
// linkage (not an anonymous namespace): glaze's compile-time reflection takes
// the type's mangled name, which MSVC refuses for internal-linkage types
// (error C7631).
namespace tnrp::detail::tnrd_v4_reader {
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
struct AeroStatusFields {
    std::string type;
    std::optional<int> active_aero_track_status;
};
} // namespace tnrp::detail::tnrd_v4_reader

namespace tnrp::detail {
namespace {
using namespace tnrd_v4_reader;

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

std::pair<float, float> sectorEndDistances(std::vector<LapProgressPoint> points) {
    std::stable_sort(points.begin(), points.end(), [](const auto& a, const auto& b) {
        return a.session_time < b.session_time;
    });

    // Distance at the exact sector time is interpolated from the same Lap Data
    // samples that supplied current_lap_ms. This mirrors the Analysis charts
    // and avoids placing a boundary one menu-rate packet late.
    std::vector<LapProgressPoint> monotonic;
    monotonic.reserve(points.size());
    for (const auto& point : points) {
        if (!std::isfinite(point.session_time) || !std::isfinite(point.lap_distance_m) ||
            point.current_lap_ms < 0 || point.lap_distance_m < 0.0f) continue;
        if (!monotonic.empty()) {
            const auto& previous = monotonic.back();
            if (point.current_lap_ms < previous.current_lap_ms ||
                point.lap_distance_m < previous.lap_distance_m) continue;
            if (point.session_time == previous.session_time) {
                monotonic.back() = point;
                continue;
            }
            if (point.lap_distance_m == previous.lap_distance_m) continue;
        }
        monotonic.push_back(point);
    }
    const auto interpolateDistance = [&](int elapsedMs) {
        if (monotonic.size() < 2 || elapsedMs < monotonic.front().current_lap_ms ||
            elapsedMs > monotonic.back().current_lap_ms) {
            return std::numeric_limits<float>::quiet_NaN();
        }
        const auto after = std::lower_bound(monotonic.begin(), monotonic.end(), elapsedMs,
            [](const LapProgressPoint& point, int value) {
                return point.current_lap_ms < value;
            });
        if (after == monotonic.begin()) return after->lap_distance_m;
        if (after == monotonic.end()) return monotonic.back().lap_distance_m;
        const auto& before = *std::prev(after);
        const int span = after->current_lap_ms - before.current_lap_ms;
        const float ratio = span > 0
            ? static_cast<float>(elapsedMs - before.current_lap_ms) / static_cast<float>(span)
            : 1.0f;
        return before.lap_distance_m + (after->lap_distance_m - before.lap_distance_m) * ratio;
    };

    float sector1 = 0.0f;
    float sector2 = 0.0f;
    bool enteredFirstSector = false;
    for (const auto& point : points) {
        if (!std::isfinite(point.lap_distance_m)) continue;
        // Race Lap 1 begins behind the timing line and can initially report
        // Sector 3 from the preceding lap. Wait for a real Sector 1 sample.
        if (!enteredFirstSector) {
            if (point.sector == 0 && point.lap_distance_m >= 0.0f)
                enteredFirstSector = true;
            continue;
        }
        if (sector1 == 0.0f && point.sector >= 1) {
            if (point.s1_ms <= 0) continue;
            const float distance = interpolateDistance(point.s1_ms);
            if (std::isfinite(distance) && distance > 0.0f) sector1 = distance;
        }
        if (sector1 > 0.0f && sector2 == 0.0f && point.sector >= 2) {
            if (point.s1_ms <= 0 || point.s2_ms <= 0) continue;
            const float distance = interpolateDistance(point.s1_ms + point.s2_ms);
            if (std::isfinite(distance) && distance > sector1) sector2 = distance;
        }
        if (sector1 > 0.0f && sector2 > sector1) break;
    }
    return {sector1, sector2};
}

// Appends a Session row's track status when it differs from the last one.
void appendTrackStatus(std::vector<TrackStatusChange>& out, float sessionTime,
                       std::string_view json) {
    AeroStatusFields fields;
    if (glz::read<kPartialRead>(fields, json) || fields.type != "session" ||
        !fields.active_aero_track_status || *fields.active_aero_track_status < 0) return;
    const int status = *fields.active_aero_track_status == 1 ? 1 : 0;
    if (!out.empty() && out.back().status == status) return;
    out.push_back({sessionTime, status});
}


// Row type and decode order of a playback-lane chunk, by index into the
// archive's chunk directory. An out-of-range index resolves to nothing rather
// than reading past the directory.
class PlaybackChunkLookup {
public:
    explicit PlaybackChunkLookup(const TnrdIndexedArchive& archive) : chunks_(archive.chunks()) {}

    bool rowType(size_t index, uint16_t& out) const {
        if (index >= chunks_.size()) return false;
        out = chunks_[index].rowType;
        return true;
    }
    uint64_t sequence(size_t index) const {
        return index < chunks_.size() ? chunks_[index].sequence : 0;
    }

private:
    const std::vector<V4ChunkInfo>& chunks_;
};

class TnrdV4Reader final : public TnrdReaderBackend {
public:
    explicit TnrdV4Reader(TnrdReaderContext& context)
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
    // TNRD_V4 recordings have no column history.
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
    const std::vector<std::string>& raceEventLines() const override { return scannedEvents_; }

    ~TnrdV4Reader() override;

    bool load(const std::string& path, HeaderRow& outHeader) override;
    bool isLoaded() const override { return indexedArchive_ && indexedArchive_->isOpen(); }

    bool hasMore() const override;
    void setCursor(float t) override;
    void beginCursorPrime(float t) override { setCursor(t); }
    void primeCursor() override;
    void playbackRowMaskChanged(float cursorTime) override {
        if (indexedArchive_) setCursor(cursorTime);
    }
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
    std::vector<TrackStatusChange> aeroTrackStatusTimeline() const override;

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

    static constexpr TnrdFormat loadedFormat_ = TnrdFormat::ChunkedV4;
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


    struct PlaybackRow {
        float t;
        uint64_t sequence;
        std::string json;
    };
    struct PlaybackLane {
        std::vector<size_t> chunks;
        size_t nextChunk{};
        bool nextPrefetched{};
        std::vector<PlaybackRow> rows;
        size_t rowPos{};
        float maxDecodedTime{};
        float safeThrough{};
    };
    struct PackedSeekCacheEntry {
        std::vector<uint8_t> bytes;
        std::list<uint64_t>::iterator lru;
    };
    static constexpr size_t PACKED_SEEK_CACHE_LIMIT = 32ull * 1024ull * 1024ull;
    // V4 chunks carry no exact time bounds until decoded.
    static constexpr bool exactIndex = false;

    bool rebuildLapCatalog();
    bool buildSectorDistanceMetadata();
    bool forEachIndexedRange(
        float fromTime, float toTime, uint32_t rowTypeMask,
        const std::function<bool(const V4TimedRow&)>& callback,
        const CancelCheck& cancelled = {});
    void preparePlaybackLap();
    bool loadPlaybackFrontier(float throughTime);
    void prefetchPlaybackChunk();
    bool encodeHotRow(uint8_t type, std::string_view json, std::vector<uint8_t>& out);
    bool encodeHotRowCached(const V4TimedRow& row, std::vector<uint8_t>& out);

    std::unique_ptr<TnrdIndexedArchive> indexedArchive_;
    std::list<uint64_t> packedSeekLru_;
    std::unordered_map<uint64_t, PackedSeekCacheEntry> packedSeekCache_;
    size_t packedSeekCacheBytes_ = 0;
    std::array<PlaybackLane, 25> playbackLanes_;
    int         playbackLap_ = 0;
    float       playbackCursor_ = 0.0f;
    bool        playbackPrepared_ = false;
    bool        playbackPrefetchOutstanding_ = false;
    TimedRaw    playbackDamageState_;
    bool        playbackDamageStateReady_ = false;
};

TnrdV4Reader::~TnrdV4Reader() {
    std::fprintf(stderr, "[close-trace] resetting indexed archive\n");
    std::fflush(stderr);
    indexedArchive_.reset();
    std::fprintf(stderr, "[close-trace] indexed archive reset; retiring bounded seek caches\n");
    std::fflush(stderr);
    // Seek buffers can still be moderately large after an All Laps request.
    // Retire them away from the caller so closing playback remains responsive.
    std::thread([
        seekCache = std::move(packedSeekCache_),
        seekLru = std::move(packedSeekLru_),
        playbackLanes = std::move(playbackLanes_)
    ]() mutable {
        std::fprintf(stderr, "[close-trace] background indexed-cache release started\n");
        std::fflush(stderr);
        seekCache.clear();
        seekLru.clear();
        playbackLanes = {};
        std::fprintf(stderr, "[close-trace] background indexed-cache release complete\n");
        std::fflush(stderr);
    }).detach();
}

bool TnrdV4Reader::load(const std::string& path, HeaderRow& outHeader) {
    V4LoadResult loaded;
    if (!TNRD_V4::load(path, loaded, lastError_)) return false;
    outHeader = std::move(loaded.header);
    indexedArchive_ = std::move(loaded.archive);
        trackLengthM_ = outHeader.track_length_m.value_or(0);
        startTime_ = indexedArchive_->startTime();
        totalTime_ = indexedArchive_->totalTime();
        initialFuelKg_ = indexedArchive_->summary().initialFuelKg;
        scannedEvents_.clear();
        scannedEvents_.reserve(indexedArchive_->summary().events.size());
        for (std::string event : indexedArchive_->summary().events) {
            while (!event.empty() && (event.back() == '\n' || event.back() == '\r'))
                event.pop_back();
            glz::generic parsedEvent;
            if (event.empty() || glz::read_json(parsedEvent, event)) {
                lastError_ = "The indexed recording contains an invalid race-event summary.";
                return false;
            }
            scannedEvents_.push_back(std::move(event));
        }
    (void)rebuildLapCatalog();
        for (const auto& s : indexedArchive_->summary().lapStatus) {
            auto it=lapBlocks_.find((int)s.lapNumber);if(it!=lapBlocks_.end())it->second.slimStatus.push_back({"status",s.sessionTime,s.ersPct,s.tyreCompound,s.visualCompound});
        }
        strategyProtocol_ = static_cast<uint16_t>(outHeader.protocol >= 2024 ? outHeader.protocol : 2025);
    if (!buildSectorDistanceMetadata()) {
        std::fprintf(stderr, "[tnrd] load FAILED: sector metadata scan failed for '%s': %s\n",
                     path.c_str(), lastError_.c_str());
        return false;
    }

        {
            // V4 lacks V5's persisted exact row/time index. Walk only the cold
            // strategy families and retain one processor checkpoint per lap,
            // then release decoded payloads and worker scratch. V5 deliberately
            // preserves its payload-lazy load path and builds these checkpoints
            // only when strategy state is first requested.
            lastError_.clear();
            StrategyProcessor completeStrategy(strategyProtocol_);
            (void)strategySnapshotAt(totalTime_, &completeStrategy);
        if (!lastError_.empty()) {
            std::fprintf(stderr, "[tnrd] load FAILED: strategy checkpoint scan failed for '%s': %s\n",
                         path.c_str(), lastError_.c_str());
            return false;
        }
            indexedArchive_->releaseTransientMemory();
        }
        setCursor(startTime_);
            std::fprintf(stderr,"[tnrd] load OK: format=%s chunks=%zu laps=%zu strategyCheckpoints=%zu cacheBytes=%zu start=%.2f total=%.2f track='%s' session='%s'\n",toString(loadedFormat_),indexedArchive_->chunks().size(),indexedArchive_->laps().size(),strategyCheckpoints_.size(),indexedArchive_->cacheBytes(),startTime_,totalTime_,outHeader.track_name.c_str(),outHeader.session_name.c_str());
    return true;
}

bool TnrdV4Reader::hasMore() const {
    if (!indexedArchive_ || !indexedArchive_->isOpen()) return false;
    if(!playbackPrepared_)return true;
    for(const auto& lane:playbackLanes_)if(lane.rowPos<lane.rows.size()||lane.nextChunk<lane.chunks.size())return true;
    const auto& laps=indexedArchive_->laps();if(playbackLap_==0)return !laps.empty();const auto it=std::find_if(laps.begin(),laps.end(),[&](const auto&l){return (int)l.lapNumber==playbackLap_;});return it!=laps.end()&&std::next(it)!=laps.end();
}

void TnrdV4Reader::setCursor(float t) {
        playbackLap_ = indexedArchive_ ? indexedArchive_->lapAt(t) : 0;
        playbackCursor_ = t;
        if(indexedArchive_)indexedArchive_->cancelPrefetch();
        playbackPrepared_ = false;
        playbackPrefetchOutstanding_ = false;
        if(!playbackDamageStateReady_||playbackDamageState_.t>t){playbackDamageState_={};playbackDamageStateReady_=false;}
    damageCadenceCursor_ = t;
}

std::vector<std::string> TnrdV4Reader::damageRowsAtCadence(
    float fromTime, float toTime, bool includeFrom,
    const CancelCheck& cancelled) const {
    std::vector<std::string> out;
    if(cancelled&&cancelled())return out;
    std::vector<TimedRaw> v4Damage;
    if (indexedArchive_) {
        std::vector<detail::V4TimedRow> rows, initial;
        std::string error;
        auto* archive = const_cast<TnrdIndexedArchive*>(indexedArchive_.get());
        if (!archive->latestRows(fromTime, {3}, initial, &error, cancelled) ||
            !archive->rowsForRange(fromTime, toTime, detail::v4TypeBit(3),
                                   rows, &error, cancelled)) return out;
        if (!initial.empty())
            v4Damage.push_back({initial.back().sessionTime, std::move(initial.back().json)});
        for (auto& row : rows)
            if (v4Damage.empty() || row.sessionTime != v4Damage.back().t ||
                row.json != v4Damage.back().json)
                v4Damage.push_back({row.sessionTime, std::move(row.json)});
    }
    const auto& damage = v4Damage;
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

bool TnrdV4Reader::rebuildLapCatalog() {
    lapBlocks_.clear();
    scannedLaps_.clear();
    fastestLapNum_ = 0;
    fastestLapMs_ = 0;
    if (!indexedArchive_) return false;
    for (const auto& lap : indexedArchive_->laps()) {
        LapBlock block{};
        block.lapNum = static_cast<int>(lap.lapNumber);
        block.startSessionTime = lap.startSessionTime;
        block.endSessionTime = lap.endSessionTime;
        lapBlocks_.emplace(block.lapNum, std::move(block));
        scannedLaps_.push_back({static_cast<int>(lap.lapNumber), lap.startSessionTime,
                                lap.endSessionTime, static_cast<int>(lap.lapTimeMs)});
        if (lap.lapTimeMs && (!fastestLapMs_ || static_cast<int>(lap.lapTimeMs) < fastestLapMs_)) {
            fastestLapMs_ = static_cast<int>(lap.lapTimeMs);
            fastestLapNum_ = static_cast<int>(lap.lapNumber);
        }
    }
    return true;
}

bool TnrdV4Reader::buildSectorDistanceMetadata() {
    for (auto& [_, block] : lapBlocks_) {
        block.sector1EndDistanceM = 0.0f;
        block.sector2EndDistanceM = 0.0f;
    }

    std::map<int, std::vector<LapProgressPoint>> progressByLap;
    {
        const auto appendLapRow = [&](float time, std::string_view json) {
            LapScanFields lap{};
            (void)glz::read<kPartialRead>(lap, json);
            if (!std::isfinite(time) || !std::isfinite(lap.lap_distance_m) ||
                lap.lap_distance_m < 0.0f) return;
            auto block = lapBlocks_.find(lap.lap_num);
            if (block == lapBlocks_.end() || time < block->second.startSessionTime ||
                time > block->second.endSessionTime) return;
            progressByLap[lap.lap_num].push_back({
                time, lap.current_lap_ms, lap.lap_distance_m,
                lap.sector, lap.s1_ms, lap.s2_ms
            });
        };

        std::string callbackError;
        std::string archiveError;
        const bool walked = indexedArchive_ && indexedArchive_->forEachChunk(
            detail::v4TypeBit(4),
            [&](const detail::V4ChunkInfo&, std::string_view plain) {
                size_t pos = 0;
                while (pos < plain.size()) {
                    size_t nl = plain.find('\n', pos);
                    if (nl == std::string_view::npos) nl = plain.size();
                    if (nl > pos) {
                        const std::string_view line = plain.substr(pos, nl - pos);
                        const float time = scanSessionTime(line.data(), static_cast<int>(line.size()));
                        if (!std::isfinite(time) || time < 0.0f) {
                            callbackError = "sector metadata contains a lap row without a valid session_time";
                            return false;
                        }
                        appendLapRow(time, line);
                    }
                    if (nl == plain.size()) break;
                    pos = nl + 1;
                }
                return true;
            }, &archiveError);
        if (!walked) {
            lastError_ = callbackError.empty()
                ? (archiveError.empty() ? "The recording's lap rows could not be scanned." : archiveError)
                : callbackError;
            return false;
        }
    }

    for (auto& [lapNum, progress] : progressByLap) {
        const auto block = lapBlocks_.find(lapNum);
        if (block == lapBlocks_.end()) continue;
        const auto [sector1, sector2] = sectorEndDistances(std::move(progress));
        block->second.sector1EndDistanceM = sector1;
        block->second.sector2EndDistanceM = sector2;
    }
    return true;
}

void TnrdV4Reader::primeCursor() {
    if (!indexedArchive_) return;
    // Seek/history extraction has normally populated time bounds and the
    // raw-chunk cache for the target. Prepare that frontier while the engine's
    // seek generation is still gated, instead of charging it to the first
    // 16 ms playback tick after the target snapshot is visible.
    (void)pullUntil(playbackCursor_);
}

std::vector<std::pair<uint8_t, std::string>> TnrdV4Reader::latestOfTypesTagged(
    float t, const std::vector<uint8_t>& types, const CancelCheck& cancelled) {
    std::vector<std::pair<uint8_t, std::string>> out;
    if(cancelled&&cancelled())return out;
    if (!indexedArchive_) return out;
    std::vector<V4TimedRow> rows;
    std::string error;
    if (!indexedArchive_->latestRows(t, types, rows, &error, cancelled)) {
        if (!cancelled || !cancelled()) lastError_ = error;
        return {};
    }
    for (auto& row : rows) out.emplace_back(row.rowType, std::move(row.json));
        std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
            return scanSessionTime(a.second.data(), static_cast<int>(a.second.size())) <
                   scanSessionTime(b.second.data(), static_cast<int>(b.second.size()));
        });
        return out;
}

std::vector<std::string> TnrdV4Reader::readRange(float fromTime, float toTime) {
    std::vector<std::string> out;
    if (!indexedArchive_) return out;
    std::vector<V4TimedRow> rows;
    std::string error;
    if (!indexedArchive_->rowsForRange(fromTime, toTime, 0xFFFFFFFFu, rows, &error)) {
        lastError_ = error;
        return out;
    }
    for (auto& r : rows) out.push_back(std::move(r.json));
    return out;
}

bool TnrdV4Reader::forEachIndexedRange(
    float fromTime, float toTime, uint32_t rowTypeMask,
    const std::function<bool(const V4TimedRow&)>& callback,
    const CancelCheck& cancelled) {
    if (!indexedArchive_ ||
        !std::isfinite(fromTime) || !std::isfinite(toTime) ||
        toTime < fromTime || rowTypeMask == 0) return false;

    // Keep the transient decoded representation bounded. A V4 chunk can span
    // much of a lap and lacks persisted time bounds until first use, but this
    // still limits aggregation to one short timeline slice instead of retaining
    // every decoded JSON row from a long All Laps/strategy request at once.
    constexpr float SLICE_SECONDS = 30.0f;
    float sliceStart = fromTime;
    for (;;) {
        if (cancelled && cancelled()) return false;
        const float sliceEnd = std::min(toTime, sliceStart + SLICE_SECONDS);
        std::vector<V4TimedRow> rows;
        std::string error;
        if (!indexedArchive_->rowsForRange(sliceStart, sliceEnd, rowTypeMask,
                                            rows, &error, cancelled)) {
            if (!cancelled || !cancelled()) lastError_ = std::move(error);
            return false;
        }
        for (const auto& row : rows) {
            if (cancelled && cancelled()) return false;
            if (!callback(row)) return false;
        }
        if (sliceEnd >= toTime) return true;
        // session_time is float in every row. Advancing by one representable
        // value makes adjacent inclusive range reads neither duplicate nor
        // omit any possible timestamp.
        sliceStart = std::nextafter(sliceEnd, std::numeric_limits<float>::infinity());
    }
}

bool TnrdV4Reader::forEachStrategyRow(
    float fromTime, float toTime, bool includeFrom,
    const std::function<void(float, std::string_view)>& callback,
    const CancelCheck& cancelled) {
    if(cancelled&&cancelled())return false;
    if (toTime < fromTime) return true;
    if (!indexedArchive_) return false;
        return forEachIndexedRange(fromTime, toTime, kStrategyDependencyMask,
            [&](const V4TimedRow& row) {
                if (!includeFrom && row.sessionTime <= fromTime) return true;
                callback(row.sessionTime, row.json);
                return true;
            }, cancelled);
}

bool TnrdV4Reader::currentLapAt(float t, float& startOut, int& numOut) const {
    if (!indexedArchive_) return false;
        numOut = indexedArchive_->lapAt(t);
        for (const auto& l : indexedArchive_->laps()) {
            if (static_cast<int>(l.lapNumber) == numOut) {
                startOut = l.startSessionTime;
                return true;
            }
        }
        return false;
}

std::string TnrdV4Reader::getLapDataMessage(int lapNum, uint32_t rowTypeMask,
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
    if (!indexedArchive_) return {};
    std::vector<V4TimedRow> rows;
    std::string error;
    const uint32_t mask = rowTypeMask & (v4TypeBit(1) | v4TypeBit(2) | v4TypeBit(3) | v4TypeBit(4) |
        v4TypeBit(11) | v4TypeBit(12) | v4TypeBit(13) | v4TypeBit(7) | v4TypeBit(8) | v4TypeBit(9));
    if (!indexedArchive_->rowsForLap((uint32_t)lapNum, mask, rows, &error)) return {};
    for (const auto& r : rows) {
        // A timed practice/quali lap number is reused across the in-lap,
        // garage and following out-lap. Those rows can share an indexed chunk
        // key with the next flying attempt, so honour the indexed flying
        // interval when materialising a lap.
        if (r.sessionTime < b.startSessionTime || r.sessionTime > b.endSessionTime) continue;
        const auto type = r.rowType;
        const auto& json = r.json;
        if (type == 1) msg.telemetry.push_back(glz::raw_json{json});
        else if (type == 2) msg.statusHistory.push_back(glz::raw_json{json});
        else if (type == 3) msg.damageHistory.push_back(glz::raw_json{json});
        else if (type == 11) msg.motionHistory.push_back(glz::raw_json{json});
        else if (type == 12) msg.motionExHistory.push_back(glz::raw_json{json});
        else if (type == 7) msg.timingHistory.push_back(glz::raw_json{json});
        else if (type == 8) msg.participantsHistory.push_back(glz::raw_json{json});
        else if (type == 9) msg.allStatusHistory.push_back(glz::raw_json{json});
        else if (type == 4) {
            LapScanFields lap{};
            (void)glz::read<kPartialRead>(lap, json);
            msg.lapProgress.push_back({r.sessionTime, lap.current_lap_ms, lap.lap_distance_m,
                                       lap.sector, lap.s1_ms, lap.s2_ms});
        } else if (type == 13) {
            msg.positionsHistory.push_back(glz::raw_json{json});
            PositionsRow pos{};
            (void)glz::read<kPartialRead>(pos, json);
            const auto car = std::find_if(pos.cars.begin(), pos.cars.end(),
                [&](const auto& value) { return value.idx == pos.player_idx; });
            if (car != pos.cars.end()) msg.playerPositions.push_back({r.sessionTime, car->x, car->z});
        }
    }
    return writeJson(msg);
}

bool TnrdV4Reader::getAnalysisLapProgress(int lapNum, AnalysisLapProgress& out,
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

    if (indexedArchive_) {
        std::vector<V4TimedRow> rows;
        std::string error;
        if (!indexedArchive_->rowsForLap(static_cast<uint32_t>(lapNum), v4TypeBit(4), rows, &error))
            return false;
        for (const auto& row : rows) {
            if (row.rowType != 4 || row.sessionTime < block.startSessionTime ||
                row.sessionTime > block.endSessionTime) continue;
            LapScanFields lap{};
            (void)glz::read<kPartialRead>(lap, row.json);
            result.points.push_back({row.sessionTime, lap.current_lap_ms,
                lap.lap_distance_m, lap.sector, lap.s1_ms, lap.s2_ms});
        }
    }

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

void TnrdV4Reader::preparePlaybackLap() {
    const float inf=std::numeric_limits<float>::infinity();
    for(auto& lane:playbackLanes_){lane.chunks.clear();lane.nextChunk=0;lane.nextPrefetched=false;lane.rows.clear();lane.rowPos=0;lane.maxDecodedTime=-inf;lane.safeThrough=inf;}
    if(!indexedArchive_||!indexedArchive_->isOpen()||playbackLap_<0){playbackPrepared_=true;return;}
    const PlaybackChunkLookup lookup(*indexedArchive_);std::vector<size_t> selected;
    indexedArchive_->chunkIndicesForLap((uint32_t)playbackLap_,playbackRowMask_,selected);
    for(size_t i:selected){uint16_t rowType=0;if(lookup.rowType(i,rowType)&&rowType<playbackLanes_.size())playbackLanes_[rowType].chunks.push_back(i);}
    for(auto& lane:playbackLanes_){
        // Range/latest-at-time extraction performed by a seek records exact
        // bounds for every chunk it inspected. Chunks wholly at/before the new
        // cursor cannot contribute a future row, so begin at the first
        // intersecting/unknown chunk rather than replaying and discarding the
        // entire lap prefix. Unknown bounds retain the conservative old path.
        while(lane.nextChunk<lane.chunks.size()){
            float first=0.0f,last=0.0f;
            if(!indexedArchive_->chunkTimeBounds(lane.chunks[lane.nextChunk],first,last)||last>playbackCursor_)break;
            ++lane.nextChunk;
        }
        if(lane.nextChunk<lane.chunks.size()){
            float first=0.0f,last=0.0f;
            // V5's exact directory bounds prove that this lane cannot produce
            // anything before its next chunk. Do not synchronously decode a
            // far-future family merely to establish that fact; load it when
            // playback reaches the bound. V4 retains the conservative path.
            lane.safeThrough=exactIndex&&indexedArchive_->chunkTimeBounds(
                lane.chunks[lane.nextChunk],first,last)?first:-inf;
        }
    }
    playbackPrepared_=true;
}

bool TnrdV4Reader::loadPlaybackFrontier(float throughTime) {
    struct PendingChunk{size_t lane;size_t index;uint64_t sequence;};
    std::vector<PendingChunk> pending;const PlaybackChunkLookup lookup(*indexedArchive_);float priority=std::numeric_limits<float>::infinity();
    for(const auto& lane:playbackLanes_)if(lane.nextChunk<lane.chunks.size()&&lane.safeThrough<=throughTime)priority=std::min(priority,lane.safeThrough);
    for(size_t i=0;i<playbackLanes_.size();++i){const auto& lane=playbackLanes_[i];if(lane.nextChunk<lane.chunks.size()&&lane.safeThrough==priority){const size_t index=lane.chunks[lane.nextChunk];pending.push_back({i,index,lookup.sequence(index)});}}
    if(pending.empty())return false;
    std::sort(pending.begin(),pending.end(),[](const auto&a,const auto&b){return a.sequence<b.sequence;});
    std::vector<size_t> indices;indices.reserve(pending.size());for(const auto& item:pending)indices.push_back(item.index);
    std::vector<std::vector<detail::V4TimedRow>> decoded;std::string error;
    if(!indexedArchive_->rowsForChunks(indices,decoded,&error)){lastError_=error;return false;}
    // The app recorder accepts at most 200 ms of packet reordering before it
    // rewinds the indexed timeline. Hold that tail until the successor is known.
    const auto block=lapBlocks_.find(playbackLap_);const float inf=std::numeric_limits<float>::infinity();constexpr float REORDER_WINDOW_S=0.2f;
    auto before=[](const PlaybackRow&a,const PlaybackRow&b){if(a.t!=b.t)return a.t<b.t;return a.sequence<b.sequence;};
    for(size_t i=0;i<pending.size();++i){
        auto& lane=playbackLanes_[pending[i].lane];
        lane.nextPrefetched=false;
        if(lane.rowPos){lane.rows.erase(lane.rows.begin(),lane.rows.begin()+(ptrdiff_t)lane.rowPos);lane.rowPos=0;}
        const size_t retained=lane.rows.size();
        for(auto& row:decoded[i]){
            const bool inBlock=block==lapBlocks_.end()||(row.sessionTime>=block->second.startSessionTime&&row.sessionTime<=block->second.endSessionTime);
            if(!inBlock)continue;if(std::isfinite(row.sessionTime))lane.maxDecodedTime=std::max(lane.maxDecodedTime,row.sessionTime);
            if(pending[i].lane==3&&row.sessionTime<=playbackCursor_&&(!playbackDamageStateReady_||row.sessionTime>=playbackDamageState_.t)){playbackDamageState_={row.sessionTime,std::move(row.json)};playbackDamageStateReady_=true;}
            else if(row.sessionTime>playbackCursor_)lane.rows.push_back({row.sessionTime,row.sequence,std::move(row.json)});
        }
        std::inplace_merge(lane.rows.begin(),lane.rows.begin()+(ptrdiff_t)retained,lane.rows.end(),before);
        ++lane.nextChunk;
        if(lane.nextChunk==lane.chunks.size())lane.safeThrough=inf;
        else if(exactIndex){
            float first=0.0f,last=0.0f;
            lane.safeThrough=indexedArchive_->chunkTimeBounds(
                lane.chunks[lane.nextChunk],first,last)?first:lane.maxDecodedTime-REORDER_WINDOW_S;
        }else lane.safeThrough=lane.maxDecodedTime-REORDER_WINDOW_S;
    }
    playbackPrefetchOutstanding_=std::any_of(playbackLanes_.begin(),playbackLanes_.end(),[](const auto& lane){return lane.nextPrefetched;});
    prefetchPlaybackChunk();
    return true;
}

void TnrdV4Reader::prefetchPlaybackChunk() {
    if(!indexedArchive_)return;const PlaybackChunkLookup lookup(*indexedArchive_);const size_t limit=exactIndex?4u:1u;size_t outstanding=0;for(const auto& lane:playbackLanes_)if(lane.nextPrefetched)++outstanding;if(outstanding>=limit){playbackPrefetchOutstanding_=true;return;}
    std::vector<size_t> candidates;for(size_t i=0;i<playbackLanes_.size();++i){const auto& lane=playbackLanes_[i];if(!lane.nextPrefetched&&lane.nextChunk<lane.chunks.size())candidates.push_back(i);}std::stable_sort(candidates.begin(),candidates.end(),[&](size_t a,size_t b){const auto& left=playbackLanes_[a];const auto& right=playbackLanes_[b];if(left.safeThrough!=right.safeThrough)return left.safeThrough<right.safeThrough;return lookup.sequence(left.chunks[left.nextChunk])<lookup.sequence(right.chunks[right.nextChunk]);});
    for(size_t laneIndex:candidates){if(outstanding>=limit)break;auto& lane=playbackLanes_[laneIndex];lane.nextPrefetched=true;++outstanding;indexedArchive_->prefetchChunk(lane.chunks[lane.nextChunk]);}playbackPrefetchOutstanding_=outstanding!=0;
}

bool TnrdV4Reader::encodeHotRow(uint8_t type,std::string_view json,std::vector<uint8_t>& out){
    if(type==1){TelemetryRow row{};if(glz::read<kPartialRead>(row,json))return false;row.rev_lights_pct.reset();row.rev_lights_bit_value.reset();bin::encodeTelemetry(out,row);return true;}
    if(type==11){MotionRow row{};if(glz::read<kPartialRead>(row,json))return false;bin::encodeMotion(out,row);return true;}
    if(type==12){MotionExRow row{};if(glz::read<kPartialRead>(row,json))return false;bin::encodeMotionEx(out,row);return true;}
    return false;
}

bool TnrdV4Reader::encodeHotRowCached(const V4TimedRow& row,std::vector<uint8_t>& out){
    const uint64_t key=(row.sequence<<32)|row.sourceOffset;auto hit=packedSeekCache_.find(key);
    if(hit!=packedSeekCache_.end()){packedSeekLru_.splice(packedSeekLru_.begin(),packedSeekLru_,hit->second.lru);hit->second.lru=packedSeekLru_.begin();out.insert(out.end(),hit->second.bytes.begin(),hit->second.bytes.end());return true;}
    std::vector<uint8_t> encoded;if(!encodeHotRow(row.rowType,row.json,encoded))return false;out.insert(out.end(),encoded.begin(),encoded.end());if(encoded.size()>PACKED_SEEK_CACHE_LIMIT)return true;
    while(packedSeekCacheBytes_+encoded.size()>PACKED_SEEK_CACHE_LIMIT&&!packedSeekLru_.empty()){const uint64_t old=packedSeekLru_.back();packedSeekLru_.pop_back();auto oldEntry=packedSeekCache_.find(old);packedSeekCacheBytes_-=oldEntry->second.bytes.size();packedSeekCache_.erase(oldEntry);}packedSeekLru_.push_front(key);packedSeekCacheBytes_+=encoded.size();packedSeekCache_.emplace(key,PackedSeekCacheEntry{std::move(encoded),packedSeekLru_.begin()});return true;
}

std::vector<std::string> TnrdV4Reader::pullUntil(float t) {
    std::vector<std::string> out;
    if(!indexedArchive_)return out;
        if(!playbackPrepared_)preparePlaybackLap();
        const float inf=std::numeric_limits<float>::infinity();
        auto before=[](const PlaybackRow&a,const PlaybackRow&b){if(a.t!=b.t)return a.t<b.t;return a.sequence<b.sequence;};
        for(;;){
            size_t best=playbackLanes_.size();float safeThrough=inf;bool futureChunk=false;
            for(size_t i=0;i<playbackLanes_.size();++i){const auto& lane=playbackLanes_[i];safeThrough=std::min(safeThrough,lane.safeThrough);futureChunk|=lane.nextChunk<lane.chunks.size();if(lane.rowPos<lane.rows.size()&&(best==playbackLanes_.size()||before(lane.rows[lane.rowPos],playbackLanes_[best].rows[playbackLanes_[best].rowPos])))best=i;}
            if(best!=playbackLanes_.size()&&playbackLanes_[best].rows[playbackLanes_[best].rowPos].t<=t&&(playbackLanes_[best].rows[playbackLanes_[best].rowPos].t<safeThrough||!futureChunk)){auto& lane=playbackLanes_[best];auto& row=lane.rows[lane.rowPos++];out.push_back(std::move(row.json));continue;}
            const float through=best!=playbackLanes_.size()&&playbackLanes_[best].rows[playbackLanes_[best].rowPos].t<=t?playbackLanes_[best].rows[playbackLanes_[best].rowPos].t:t;
            if(futureChunk&&safeThrough<=through){if(loadPlaybackFrontier(through))continue;break;}
            if(best!=playbackLanes_.size())break;
            const auto& laps=indexedArchive_->laps();auto next=playbackLap_==0?laps.begin():std::find_if(laps.begin(),laps.end(),[&](const auto& lap){return (int)lap.lapNumber==playbackLap_;});if(playbackLap_!=0&&next!=laps.end())++next;if(next==laps.end()||next->startSessionTime>t)break;indexedArchive_->cancelPrefetch();playbackPrefetchOutstanding_=false;playbackLap_=(int)next->lapNumber;playbackPrepared_=false;preparePlaybackLap();
        }
        prefetchPlaybackChunk();return out;
}

void TnrdV4Reader::pullUntilSplit(float t, std::string& jsonOut, std::vector<uint8_t>& binOut,
                             uint32_t& seenTypes,
                             std::array<std::string, 16>* lastOfType) {
    const float cadenceEnd = std::isfinite(t) ? t : totalTime_;
    if(!indexedArchive_)return;
    // V1-V5 recordings deduplicated unchanged damage state, so their readers
    // reconstruct the original 10 Hz stream.
    constexpr bool reconstructDamage = true;
        if(reconstructDamage&&!playbackDamageStateReady_&&lastOfType&&!(*lastOfType)[3].empty()){const float seedTime=scanSessionTime((*lastOfType)[3].data(),(int)(*lastOfType)[3].size());if(seedTime<=damageCadenceCursor_){playbackDamageState_={seedTime,(*lastOfType)[3]};playbackDamageStateReady_=true;}}
        std::vector<TimedRaw> damageUpdates;auto rows=pullUntil(t);
        for(auto&row:rows){const uint8_t tid=scanType(row.data(),(int)row.size());if(!(playbackRowMask_&(1u<<tid)))continue;seenTypes|=(1u<<tid);if(tid==1||tid==11||tid==12){(void)encodeHotRow(tid,row,binOut);continue;}if(tid==3&&reconstructDamage){if(lastOfType)(*lastOfType)[3]=row;damageUpdates.push_back({scanSessionTime(row.data(),(int)row.size()),std::move(row)});continue;}jsonOut+=row;jsonOut.push_back('\n');if(lastOfType&&tid<lastOfType->size())(*lastOfType)[tid]=row;}
        if(reconstructDamage&&(playbackRowMask_&(1u<<3))&&std::isfinite(damageCadenceCursor_)&&std::isfinite(cadenceEnd)&&cadenceEnd>=damageCadenceCursor_){
            constexpr double RATE=10.0,EPS=1e-6;const long long firstTick=(long long)std::floor((double)damageCadenceCursor_*RATE+EPS)+1,lastTick=(long long)std::floor((double)cadenceEnd*RATE+EPS);size_t update=0;
            for(long long tick=firstTick;tick<=lastTick;++tick){const float sampleTime=(float)((double)tick/RATE);while(update<damageUpdates.size()&&damageUpdates[update].t<=sampleTime+(float)EPS){playbackDamageState_=std::move(damageUpdates[update++]);playbackDamageStateReady_=true;}if(!playbackDamageStateReady_)continue;std::string row=playbackDamageState_.json;setSessionTime(row,sampleTime);jsonOut+=row;jsonOut.push_back('\n');seenTypes|=(1u<<3);if(lastOfType)(*lastOfType)[3]=row;}
            while(update<damageUpdates.size()){playbackDamageState_=std::move(damageUpdates[update++]);playbackDamageStateReady_=true;}
        }
        damageCadenceCursor_=cadenceEnd;return;
}

TnrdV4Reader::SeekFlush TnrdV4Reader::seekFlush(float target, float currentLapStart,
                                      bool allHistory, uint32_t requestedTypes,
                                      float windowSeconds, bool includeMandatoryState,
                                      const CancelCheck& cancelled) {
    SeekFlush f;
    // Comparison seeks restore the current lap, finite time-window seeks use
    // their exact session-time prefix, and AL asks for the full indexed prefix.
    // Every mode travels through the same packed flush path.
    const float windowStart = allHistory
        ? startTime_
        : windowSeconds > 0.0f
            ? std::max(startTime_, target - windowSeconds)
            : currentLapStart;
    const uint32_t mandatory = detail::v4TypeBit(2) |
        detail::v4TypeBit(3) | detail::v4TypeBit(4);
    const uint32_t mask = (allHistory || !includeMandatoryState)
        ? requestedTypes : (requestedTypes | mandatory);

    if (!indexedArchive_) return f;
        const bool currentLapOnly=!allHistory&&windowSeconds<=0.0f;
        // V1-V5 recordings deduplicated unchanged damage state, so their
        // readers reconstruct the original 10 Hz stream. V6 persists every
        // damage packet and reads it directly like every other cold family.
    constexpr bool reconstructDamage = true;
        const uint32_t directMask = reconstructDamage
            ? mask & ~detail::v4TypeBit(3) : mask;
        if(cancelled&&cancelled())return f;
        auto binary=std::make_shared<std::vector<uint8_t>>();
        const auto appendRow = [&](const V4TimedRow& row) {
            if (row.rowType == 1 || row.rowType == 11 || row.rowType == 12)
                return encodeHotRowCached(row, *binary);
            f.coldJson += row.json;
            f.coldJson.push_back('\n');
            return true;
        };
        bool loaded = true;
    if (directMask && currentLapOnly) {
            std::vector<V4TimedRow> rows;
            std::string error;
            loaded = indexedArchive_->rowsForLapRange(
                (uint32_t)indexedArchive_->lapAt(target), windowStart, target,
                directMask, rows, &error, cancelled);
            if (!loaded) {
                if (!cancelled || !cancelled()) lastError_ = std::move(error);
            } else {
                for (const auto& row : rows)
                    if (!appendRow(row)) { loaded = false; break; }
            }
        } else if (directMask) {
            loaded = forEachIndexedRange(windowStart, target, directMask,
                                         appendRow, cancelled);
        }
        if(!loaded)return f;
        if(cancelled&&cancelled())return {};
        if(reconstructDamage&&(mask&detail::v4TypeBit(3))){auto damage=damageRowsAtCadence(windowStart,target,true,cancelled);if(cancelled&&cancelled())return {};if(!damage.empty()){playbackDamageState_={scanSessionTime(damage.back().data(),(int)damage.back().size()),damage.back()};playbackDamageStateReady_=true;}for(auto&row:damage){f.coldJson+=row;f.coldJson.push_back('\n');}}if(!f.coldJson.empty())f.coldJson.pop_back();if(!binary->empty()){f.binaryStore=binary;f.binaryBegin=0;f.binaryEnd=binary->size();}return f;
}

std::vector<TrackStatusChange> TnrdV4Reader::aeroTrackStatusTimeline() const {
    std::vector<TrackStatusChange> timeline;
    if (!indexedArchive_) return timeline;
    if (lapBlocks_.empty()) return timeline;
    std::vector<V4TimedRow> rows;
    std::string error;
    if (!const_cast<TnrdIndexedArchive*>(indexedArchive_.get())->rowsForRange(
            lapBlocks_.begin()->second.startSessionTime, lapBlocks_.rbegin()->second.endSessionTime,
            detail::v4TypeBit(5), rows, &error)) return timeline;
    std::stable_sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
        return a.sessionTime < b.sessionTime;
    });
    for (const auto& row : rows) appendTrackStatus(timeline, row.sessionTime, row.json);
    return timeline;
}

StrategySnapshotRow TnrdV4Reader::strategySnapshotAt(float t, StrategyProcessor* restoredProcessor,
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

std::string TnrdV4Reader::lapBlocksMessage() const {
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
    msg.tnrdVersion = "TNRD_V4";
    msg.deltaAvailable = true;
    msg.lapDistanceAvailable = true;
    msg.trackLengthM = trackLengthM_;
    msg.events.reserve(scannedEvents_.size());
    for (const auto& s : scannedEvents_) msg.events.push_back(glz::raw_json{ s });
    for (const auto& l : scannedLaps_)   msg.laps.push_back({ l.lapNum, l.lapTimeMs });
    return writeJson(msg);
}

std::vector<int> TnrdV4Reader::analysisLapNumbers(int driverIndex) const {
    (void)driverIndex;
    std::vector<int> laps;
    for (const auto& [lapNum, block] : lapBlocks_) laps.push_back(lapNum);
    return laps;
}

bool TnrdV4Reader::getAnalysisErsSamples(int lapNum, std::vector<ErsSample>& out,
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

bool TnrdV4Reader::getAnalysisHarvestSamples(int lapNum, std::vector<HarvestSample>& out,
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

bool TnrdV4Reader::getAnalysisInputSamples(int lapNum, std::vector<InputSample>& out,
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

bool TnrdV4Reader::getAnalysisPositionSamples(int lapNum, std::vector<PlayerPositionPoint>& out,
                                         int driverIndex) const {
    constexpr uint32_t positionRows = 1u << 13;
    const std::string json = getLapDataMessage(lapNum, positionRows, driverIndex);
    if (json.empty()) return false;
    PositionLapPayload payload;
    if (glz::read<kPartialRead>(payload, json)) return false;
    out = std::move(payload.playerPositions);
    return true;
}


// Hands every stored row to the V4 exporter, chunk by chunk (by row type,
// then lap, then sequence). Rows carry their own session_time.
bool TnrdV4Reader::exportParquet(const ParquetExportRequest& request, std::string* errorOut) {
    if (!indexedArchive_) {
        if (errorOut) *errorOut = "no .tnrd loaded";
        return false;
    }
    JsonlExportSource source;
    for (const auto& chunk : indexedArchive_->chunks()) source.rowCount += chunk.rowCount;
    source.forEach = [this](const JsonlExportSource::Visitor& visit, std::string* readError) {
        return indexedArchive_->forEachChunk(~V4RowTypeMask{},
            [&](const V4ChunkInfo&, std::string_view plain) {
                size_t pos = 0;
                while (pos < plain.size()) {
                    size_t nl = plain.find('\n', pos);
                    if (nl == std::string_view::npos) nl = plain.size();
                    if (nl > pos && !visit(std::numeric_limits<float>::quiet_NaN(), plain.substr(pos, nl - pos)))
                        return false;
                    if (nl == plain.size()) break;
                    pos = nl + 1;
                }
                return true;
            }, readError);
    };
    return exportTnrdV4Parquet(source, request, errorOut);
}

} // namespace

std::unique_ptr<TnrdReaderBackend> makeTnrdV4Reader(TnrdReaderContext& context) {
    return std::make_unique<TnrdV4Reader>(context);
}

bool summarizeTnrdV4(const std::string& path, TnrdReader::Summary& out, std::string& error) {
    V4LoadResult loaded;
    if (!TNRD_V4::load(path, loaded, error)) return false;
    out.header = std::move(loaded.header);
    out.durationSeconds = std::max(0.0f, loaded.archive->totalTime() - loaded.archive->startTime());
    return true;
}

} // namespace tnrp::detail
