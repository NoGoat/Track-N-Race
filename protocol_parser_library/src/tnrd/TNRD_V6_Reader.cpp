// TNRD_V6 playback reader. A V6 recording stores each driver's laps as
// per-field data-type chunks (Zstandard column tables) plus shared session
// records. Playback reads the selected driver's chunks and projects them into
// the legacy row families the frontends consume; Analysis reads columns
// directly.

#include "TNRD_V6.h"
#include "TnrdCodec.h"
#include "TnrdReaderBackend.h"
#include "../PairLapData.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <system_error>
#include <thread>
#include <unordered_set>

#include "tnrp/BinaryRows.h"
#include "tnrp/TyreStints.h"

// Partial reads of recorded rows. glaze's error_on_unknown_keys=false ignores
// every other key; missing keys keep their defaults. These must have external
// linkage (not an anonymous namespace): glaze's compile-time reflection takes
// the type's mangled name, which MSVC refuses for internal-linkage types
// (error C7631).
namespace tnrp::detail::tnrd_v6_reader {
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
struct AeroStatusFields {
    std::string type;
    std::optional<int> active_aero_track_status;
};
} // namespace tnrp::detail::tnrd_v6_reader

namespace tnrp::detail {
namespace {
using namespace tnrd_v6_reader;

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


std::vector<TyreStintRange> v6TyreStints(const V6DriverHeader& driver) {
    return tyreStintRanges(driver.tyreStints, [](const V6TyreStintSummary& stint) {
        return SessionHistoryTyreStint{stint.endLap, stint.actualCompound, stint.visualCompound};
    });
}

// The compound `driver` had fitted at `time`, from its recorded Car Status tyre
// state, for a lap no Session History stint covers: in Grand Prix qualifying the
// game reports the set in use as an empty stint instead of an open one (end lap
// 255). Every lap holds this state from its start, so the latest value at or
// before the lap's end is the set it ran on, even when its start was repaired
// past that sample.
std::optional<StatusScanFields> v6CompoundAt(TnrdV6Archive& v6, uint8_t driver, float time) {
    std::vector<V6TimedRow> rows;
    if (!v6.readMultiDriverLatest(time, {driver}, {static_cast<uint8_t>(V6DataType::TyreState)},
                                  rows, nullptr)) return std::nullopt;
    for (const auto& row : rows) {
        StatusScanFields fields;
        if (!glz::read<kPartialRead>(fields, row.json) && fields.tyre_compound > 0) return fields;
    }
    return std::nullopt;
}

// Recordings made before the parser filled avg_wear_per_lap carry only the raw
// sets. Derive it against the car's first recorded sets, the same session
// baseline the live parser uses, and splice the sets back into `row`.
void fillRecordedTyreSetWear(std::string& row, std::string_view json,
                             const std::vector<TyreSet>& baseline) {
    if (baseline.empty() || json.find("\"avg_wear_per_lap\"") != std::string_view::npos) return;
    TyreSetsRow parsed;
    if (glz::read<kPartialRead>(parsed, json)) return;
    bool filled = false;
    for (auto& set : parsed.sets) {
        const auto base = std::find_if(baseline.begin(), baseline.end(),
            [&](const TyreSet& candidate) { return candidate.idx == set.idx; });
        if (base == baseline.end() || base->actual_compound != set.actual_compound ||
            set.wear < base->wear || set.life_span > base->life_span) continue;
        const int laps = base->life_span - set.life_span;
        if (laps <= 0) continue;
        set.avg_wear_per_lap = std::round((set.wear - base->wear) * 100.0 / laps) / 100.0;
        filled = true;
    }
    if (!filled) return;
    const auto key = row.find("\"sets\":");
    if (key == std::string::npos) return;
    const size_t begin = key + 7;
    if (begin >= row.size() || row[begin] != '[') return;
    size_t end = begin;
    for (int depth = 0; end < row.size(); ++end) {
        if (row[end] == '[') ++depth;
        else if (row[end] == ']' && --depth == 0) break;
    }
    if (end >= row.size()) return;
    std::string sets;
    (void)glz::write_json(parsed.sets, sets);
    if (!sets.empty()) row.replace(begin, end + 1 - begin, sets);
}

std::vector<uint8_t> v6TypesForRowMask(uint32_t mask) {
    std::vector<uint8_t> out;
    const auto add = [&](std::initializer_list<uint8_t> values) {
        out.insert(out.end(), values.begin(), values.end());
    };
    if (mask & v4TypeBit(1)) add({1,2,3,4,5,6,7,8,9,10,11});
    if (mask & v4TypeBit(2)) add({7,13,15,16,17,18,19,20});
    if (mask & v4TypeBit(9)) add({7,13,15,16,17,18,19,20});
    if (mask & v4TypeBit(10)) add({13});
    if (mask & v4TypeBit(3)) add({12,14});
    if (mask & (v4TypeBit(4) | v4TypeBit(7))) add({24});
    if (mask & v4TypeBit(11)) add({21});
    if (mask & v4TypeBit(12)) add({22});
    if (mask & v4TypeBit(13)) add({23});
    std::sort(out.begin(), out.end()); out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

uint8_t scanV6StoredType(std::string_view json) {
    constexpr std::string_view key = "\"_v6_type\":";
    const size_t at = json.find(key); if (at == json.npos) return 0;
    return static_cast<uint8_t>(std::strtoul(json.data() + at + key.size(), nullptr, 10));
}

int scanV6Driver(std::string_view json, int fallback) {
    constexpr std::string_view key = "\"driver_idx\":";
    const size_t at = json.find(key);
    if (at == json.npos) return fallback;
    return static_cast<int>(std::strtol(json.data() + at + key.size(), nullptr, 10));
}

void tagV6StoredType(std::string& json, uint8_t type) {
    if (json.empty() || json.front() != '{') return;
    json.insert(1, "\"_v6_type\":" + std::to_string(type) + ',');
}

struct V6ProjectedRecord {
    uint8_t type{};
    float sessionTime{};
    uint64_t order{};
    std::string json;
};

// Compatibility-only assembler for hosts that have not opted into Electron's
// sparse V6 patch contract. The V6 fast path bypasses this class completely.
class V6ProjectionAssembler {
public:
    void seed(uint8_t type, std::string_view json) {
        if (json.empty()) return;
        if (type == 7) (void)glz::read<kPartialRead>(timing_, json);
        else if (type == 9) (void)glz::read<kPartialRead>(allStatus_, json);
        else if (type == 13) (void)glz::read<kPartialRead>(positions_, json);
    }

    void add(uint8_t type, float sessionTime, std::string json) {
        if (type >= states_.size() || json.empty()) return;
        if (type == 7) {
            TimingRow patch;
            if (glz::read<kPartialRead>(patch, json)) return;
            begin(type, sessionTime);
            timing_.session_time = sessionTime;
            timing_.player_idx = patch.player_idx;
            mergeCars(timing_.cars, patch.cars);
            return;
        }
        if (type == 9) {
            AllStatusRow patch;
            if (glz::read<kPartialRead>(patch, json)) return;
            begin(type, sessionTime);
            allStatus_.session_time = sessionTime;
            for (const auto& car : patch.cars) {
                auto found = std::find_if(allStatus_.cars.begin(), allStatus_.cars.end(),
                    [&](const auto& value) { return value.idx == car.idx; });
                if (found == allStatus_.cars.end()) {
                    allStatus_.cars.push_back({});
                    found = std::prev(allStatus_.cars.end());
                    found->idx = car.idx;
                }
                switch (scanV6StoredType(json)) {
                    case 7: found->drs_allowed = car.drs_allowed; break;
                    case 13:
                        found->tyre_compound = car.tyre_compound;
                        found->visual_compound = car.visual_compound;
                        found->tyre_age_laps = car.tyre_age_laps;
                        break;
                    case 15:
                        found->fuel_kg = car.fuel_kg; found->fuel_laps = car.fuel_laps;
                        found->fuel_mix = car.fuel_mix; break;
                    case 16:
                        found->ers_j = car.ers_j; found->ers_pct = car.ers_pct;
                        found->ers_mode = car.ers_mode; break;
                    case 17:
                        found->ers_harvested_mguk_j = car.ers_harvested_mguk_j;
                        found->ers_harvested_mguh_j = car.ers_harvested_mguh_j; break;
                    case 18: found->ers_deployed_j = car.ers_deployed_j; break;
                    case 19:
                        found->engine_power_ice_kw = car.engine_power_ice_kw;
                        found->engine_power_mguk_kw = car.engine_power_mguk_kw; break;
                    case 20: found->front_brake_bias = car.front_brake_bias; break;
                    default: break;
                }
            }
            std::sort(allStatus_.cars.begin(), allStatus_.cars.end(),
                [](const auto& left, const auto& right) { return left.idx < right.idx; });
            return;
        }
        if (type == 13) {
            PositionsRow patch;
            if (glz::read<kPartialRead>(patch, json)) return;
            begin(type, sessionTime);
            positions_.player_idx = patch.player_idx;
            mergeCars(positions_.cars, patch.cars);
            return;
        }

        glz::generic patch;
        if (glz::read_json(patch, json) || !patch.is_object()) return;
        begin(type, sessionTime);
        const bool unavailable = json.find("\"available\":false") != std::string::npos;
        if (!ready_[type] || unavailable) {
            states_[type] = std::move(patch);
            ready_[type] = true;
        } else {
            auto& target = states_[type].get_object();
            target.erase("available");
            for (auto& [key, value] : patch.get_object())
                target.insert_or_assign(key, std::move(value));
        }
    }

    std::vector<V6ProjectedRecord> take() {
        for (uint8_t type = 0; type < kTypeSlots; ++type)
            if (pending_[type]) flush(type);
        std::stable_sort(records_.begin(), records_.end(), [](const auto& left, const auto& right) {
            return std::tie(left.sessionTime, left.order) < std::tie(right.sessionTime, right.order);
        });
        return std::move(records_);
    }

private:
    void begin(uint8_t type, float sessionTime) {
        if (pending_[type] && pendingTime_[type] != sessionTime) flush(type);
        if (!pending_[type]) {
            pending_[type] = true;
            pendingTime_[type] = sessionTime;
            pendingOrder_[type] = nextOrder_++;
        }
    }

    template <typename Car>
    static void mergeCars(std::vector<Car>& target, const std::vector<Car>& patch) {
        for (const auto& car : patch) {
            const auto found = std::find_if(target.begin(), target.end(),
                [&](const auto& value) { return value.idx == car.idx; });
            if (found == target.end()) target.push_back(car);
            else *found = car;
        }
        std::sort(target.begin(), target.end(),
            [](const auto& left, const auto& right) { return left.idx < right.idx; });
    }

    void flush(uint8_t type) {
        std::string merged;
        bool written = false;
        if (type == 7) written = !glz::write_json(timing_, merged);
        else if (type == 9) written = !glz::write_json(allStatus_, merged);
        else if (type == 13) written = !glz::write_json(positions_, merged);
        else written = !glz::write_json(states_[type], merged);
        if (written)
            records_.push_back({type, pendingTime_[type], pendingOrder_[type], std::move(merged)});
        pending_[type] = false;
    }

    static constexpr uint8_t kTypeSlots = 16;
    std::array<glz::generic, kTypeSlots> states_{};
    std::array<bool, kTypeSlots> ready_{};
    std::array<bool, kTypeSlots> pending_{};
    std::array<float, kTypeSlots> pendingTime_{};
    std::array<uint64_t, kTypeSlots> pendingOrder_{};
    std::vector<V6ProjectedRecord> records_;
    uint64_t nextOrder_{};
    TimingRow timing_{};
    AllStatusRow allStatus_{};
    PositionsRow positions_{};
};

// Row type and decode order of a playback-lane chunk. Chunk queries on a V6
// archive (playbackChunkIndices, chunkTimeBounds, prefetchChunk, deferred
// rows) all use indices into its full chunk directory, which spans every
// session phase. An out-of-range index resolves to nothing rather than reading
// past the directory.
class PlaybackChunkLookup {
public:
    explicit PlaybackChunkLookup(const TnrdV6Archive& archive) : chunks_(archive.v6Chunks()) {}

    bool rowType(size_t index, uint16_t& out) const {
        if (index >= chunks_.size()) return false;
        out = chunks_[index].typeId;
        return true;
    }
    uint64_t sequence(size_t index) const {
        return index < chunks_.size() ? chunks_[index].sequence : 0;
    }

private:
    const std::vector<V6ChunkInfo>& chunks_;
};

namespace {
// A number field of a V6 row, NaN when the row does not carry it.
double v6RowNumber(const std::string& json, std::string_view key) {
    const size_t at = json.find(key);
    if (at == std::string::npos) return std::numeric_limits<double>::quiet_NaN();
    const char* value = json.c_str() + at + key.size();
    char* end = nullptr;
    const double parsed = std::strtod(value, &end);
    return end == value ? std::numeric_limits<double>::quiet_NaN() : parsed;
}
} // namespace

namespace {

// Two counter columns as one series: a sample at every time either has one,
// each column carrying its latest value forward.
std::vector<HarvestSample> mergeHarvest(const std::vector<std::pair<float, double>>& mguk,
                                        const std::vector<std::pair<float, double>>& mguh) {
    std::vector<HarvestSample> out;
    out.reserve(std::max(mguk.size(), mguh.size()));
    size_t k = 0, h = 0;
    double kNow = 0.0, hNow = 0.0;
    while (k < mguk.size() || h < mguh.size()) {
        const float time = h >= mguh.size() || (k < mguk.size() && mguk[k].first <= mguh[h].first)
            ? mguk[k].first : mguh[h].first;
        while (k < mguk.size() && mguk[k].first == time) kNow = mguk[k++].second;
        while (h < mguh.size() && mguh[h].first == time) hNow = mguh[h++].second;
        if (std::isfinite(kNow) && std::isfinite(hNow)) out.push_back({time, kNow + hNow});
    }
    return out;
}

// The harvest counters of one car over [from, to], read raw from a V6 archive.
bool v6HarvestSamples(TnrdV6Archive& v6, uint8_t car, float from, float to,
                      std::vector<HarvestSample>& out, const IndexedCancelCheck& cancelled = {}) {
    std::vector<std::pair<float, double>> mguk;
    std::vector<std::pair<float, double>> mguh;
    std::string error;
    if (!v6.fieldSamples(car, V6DataType::ERSHarvest, "ers_harvested_mguk_j",
                         from, to, mguk, &error, cancelled) ||
        !v6.fieldSamples(car, V6DataType::ERSHarvest, "ers_harvested_mguh_j",
                         from, to, mguh, &error, cancelled)) return false;
    out = mergeHarvest(mguk, mguh);
    return true;
}

} // namespace

// A second read-only handle on a V6 file, for chart history while playback
// keeps the reader's own handle and chunk cache.
class V6HistoryReader final : public TnrdReader::HistoryReader {
public:
    explicit V6HistoryReader(std::unique_ptr<TnrdV6Archive> archive) : archive_(std::move(archive)) {}
    bool read(const TnrdReader::HistoryRequest& request, std::vector<uint8_t>& out,
              std::string* errorOut, const std::function<bool()>& cancelled) override {
        return archive_->columnarHistory(request.driver, request.types, request.mask, request.from,
                                         request.to, request.seed, out, errorOut, cancelled);
    }

private:
    std::unique_ptr<TnrdV6Archive> archive_;
};

class TnrdV6Reader final : public TnrdReaderBackend {
public:
    explicit TnrdV6Reader(TnrdReaderContext& context)
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

    ~TnrdV6Reader() override;

    bool load(const std::string& path, HeaderRow& outHeader) override;
    // Adopts an archive that is already open, for Strategy reconstruction
    // only: no playback cursor or sector metadata is prepared.
    void adoptForStrategy(std::unique_ptr<TnrdV6Archive> archive, const HeaderRow& header);
    bool isLoaded() const override { return indexedArchive_ && indexedArchive_->isOpen(); }
    bool wasRecoveredV6() const override { return indexedArchive_ && indexedArchive_->wasRecovered(); }

    bool hasMore() const override;
    void setCursor(float t) override;
    void beginCursorPrime(float t) override;
    void primeCursor() override;
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

    void playbackRowMaskChanged(float cursorTime) override {
        if (indexedArchive_) setCursor(cursorTime);
    }
    void playbackV6TypesChanged(float cursorTime) override;
    void setPlaybackDriver(int driverIndex, bool useRecordedRows, float cursorTime) override;
    void setV6FocusDriver(int driverIndex, float cursorTime) override;
    std::vector<std::string> v6FocusStatusSnapshot(float t) override;
    int effectivePlaybackDriver() const override {
        return playbackDriverIndex_ >= 0 ? playbackDriverIndex_ : recordedDriverIndex_;
    }

    std::string driverRestrictionMessage(float cursorTime) const override;
    std::string getLapDataMessage(int lapNum, uint32_t rowTypeMask, int driverIndex) const override;
    std::string phoneChartLapJson(int lapNum, const std::vector<std::string>& channels) const override;

    bool getAnalysisLapProgress(int lapNum, AnalysisLapProgress& out, int driverIndex) const override;
    std::vector<int> analysisLapNumbers(int driverIndex) const override;
    bool getAnalysisErsSamples(int lapNum, std::vector<ErsSample>& out, int driverIndex) const override;
    bool getAnalysisHarvestSamples(int lapNum, std::vector<HarvestSample>& out,
                                   int driverIndex) const override;
    bool getAnalysisInputSamples(int lapNum, std::vector<InputSample>& out,
                                 int driverIndex) const override;
    bool getAnalysisPositionSamples(int lapNum, std::vector<PlayerPositionPoint>& out,
                                    int driverIndex) const override;
    const std::vector<std::string>& raceEventLines() const override { return scannedEvents_; }
    std::vector<TrackStatusChange> aeroTrackStatusTimeline() const override;
    std::shared_ptr<ErsPeakSweep> startErsPeakSweep() override;
    std::shared_ptr<TrackCornerSweep> startTrackCornerSweep() override;
    DriverLapHistoryRow driverLapHistory(int driver, float throughTime) const override;
    LapMeasuresRow lapMeasures(int driver, float throughTime, bool hasMguh) const override;
    bool v6FastestLapAt(float t, FastestLapRow& out) const override;
    float v6NextLapEndAfter(float t) const override;

    std::unique_ptr<TnrdReader::HistoryReader> openHistoryReader(std::string* errorOut) const override;
    bool historyRequest(float from, float to, uint32_t mask,
                        TnrdReader::HistoryRequest& out) const override;

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

    static constexpr TnrdFormat loadedFormat_ = TnrdFormat::ChunkedV6;
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


    // A row arrives unrendered: `source` is set and `json` stays empty until
    // pullUntil() emits it (see loadPlaybackFrontier()).
    struct PlaybackRow {
        float t;
        uint64_t sequence;
        std::string json;
        std::shared_ptr<const V6RowSource> source{};
        uint32_t sourceRow{};
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
    // Each time the fastest lap improved: when the lap ended, whose it was and
    // how long it took. Built from the lap summaries on first use.
    struct V6FastestLapChange { float time; int car; int lapMs; };
    // Each car's measured laps, read from the samples around its lap lines on
    // first use: lap number -> totals, with the lap's logical end time.
    struct V6MeasuredLap { float logicalEnd; LapTotals totals; };

    // Source families playback reads for the families it emits: the same set.
    static uint32_t expandedPlaybackMask(uint32_t outputMask) { return outputMask; }
    // The fields history reads load: the host's history subscription, or its
    // stream fields when it has none (empty = every field).
    const std::vector<uint8_t>& historyV6Types() const {
        return playbackV6HistoryTypes_.empty() ? playbackV6Types_ : playbackV6HistoryTypes_;
    }
    int analysisCarIndex(int driverIndex) const {
        return driverIndex >= 0 ? driverIndex : effectivePlaybackDriver();
    }
    void addAnalysisDrivers(PlaybackLapBlocksRow& msg) const;
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
    bool v6FocusStatusRows(float t, std::vector<V4TimedRow>& out);
    // First recorded tyre sets per car, read on first use: the session wear
    // baseline for rows recorded before the parser added avg_wear_per_lap.
    const std::vector<TyreSet>& firstV6TyreSets(int car) const;
    void projectV6Row(uint8_t sourceType, float sessionTime, std::string_view json,
                      std::vector<std::pair<uint8_t, std::string>>& out,
                      int selectedDriverOverride = -1,
                      uint32_t outputMaskOverride = 0) const;

    std::unique_ptr<TnrdV6Archive> indexedArchive_;
    std::string v6Path_;   // the file loaded, for the background sweeps
    std::array<PlaybackLane, 25> playbackLanes_;
    int         playbackLap_ = 0;
    float       playbackCursor_ = 0.0f;
    bool        playbackPrepared_ = false;
    bool        playbackPrefetchOutstanding_ = false;
    int         playbackDriverIndex_ = -1;
    int         recordedDriverIndex_ = -1;
    bool        playbackDriverUsesRecordedRows_ = false;
    std::vector<size_t> v6SharedOrder_;
    size_t      v6SharedPos_ = 0;
    std::array<std::string, 16> v6ProjectionState_{};
    mutable std::array<std::optional<std::vector<TyreSet>>, 24> v6FirstTyreSets_{};
    mutable std::optional<std::vector<V6FastestLapChange>> v6FastestLapChanges_;
    mutable std::optional<std::vector<float>> v6LapEnds_;
    mutable std::array<std::optional<std::map<int, V6MeasuredLap>>, 24> v6LapMeasures_{};
    static constexpr bool exactIndex = true;
};

TnrdV6Reader::~TnrdV6Reader() {
    std::fprintf(stderr, "[close-trace] resetting indexed archive\n");
    std::fflush(stderr);
    indexedArchive_.reset();
    std::fprintf(stderr, "[close-trace] indexed archive reset; retiring playback lanes\n");
    std::fflush(stderr);
    // Playback lanes can still hold a lap of decoded rows. Retire them away
    // from the caller so closing playback remains responsive.
    std::thread([playbackLanes = std::move(playbackLanes_)]() mutable {
        playbackLanes = {};
        std::fprintf(stderr, "[close-trace] background indexed-cache release complete\n");
        std::fflush(stderr);
    }).detach();
}

bool TnrdV6Reader::load(const std::string& path, HeaderRow& outHeader) {
    V6LoadResult loaded;
    if (!TNRD_V6::load(path, loaded, lastError_)) return false;
    outHeader = std::move(loaded.header);
    indexedArchive_ = std::move(loaded.archive);
        trackLengthM_ = outHeader.track_length_m.value_or(0);
        startTime_ = indexedArchive_->startTime();
        totalTime_ = indexedArchive_->totalTime();
        initialFuelKg_ = indexedArchive_->summary().initialFuelKg;
    v6Path_ = path;
    if (auto* v6 = indexedArchive_.get()) {
                if (const auto player = v6->playerDriverIndex()) {
                    recordedDriverIndex_ = *player;
                    playbackDriverIndex_ = *player;
                    playbackDriverUsesRecordedRows_ = true;
                    v6->setPlaybackDriver(*player);
                }
                v6SharedOrder_.clear();
                for (size_t i = 0; i < v6->sharedRecords().size(); ++i)
                    if (v6->sharedRecords()[i].phase == V6Phase::Race)
                        v6SharedOrder_.push_back(i);
                std::stable_sort(v6SharedOrder_.begin(), v6SharedOrder_.end(),
                    [&](size_t left, size_t right) {
                        const auto& a = v6->sharedRecords()[left];
                        const auto& b = v6->sharedRecords()[right];
                        return v6->logicalTime(a.phase, a.sessionTime) <
                               v6->logicalTime(b.phase, b.sessionTime);
                    });
    }
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
    setCursor(startTime_);
        if (const auto* v6 = indexedArchive_.get())
            std::fprintf(stderr,"[tnrd] load OK: format=%s revision=%s chunks=%zu laps=%zu start=%.2f total=%.2f track='%s' session='%s'\n",toString(loadedFormat_),v6->revision() ? "TNRD_V6.1" : "TNRD_V6",indexedArchive_->chunks().size(),indexedArchive_->laps().size(),startTime_,totalTime_,outHeader.track_name.c_str(),outHeader.session_name.c_str());
    return true;
}

void TnrdV6Reader::adoptForStrategy(std::unique_ptr<TnrdV6Archive> archive, const HeaderRow& header) {
    const auto player = archive->playerDriverIndex();
    indexedArchive_ = std::move(archive);
    trackLengthM_ = header.track_length_m.value_or(0);
    startTime_ = indexedArchive_->startTime();
    totalTime_ = indexedArchive_->totalTime();
    if (player) {
        recordedDriverIndex_ = *player;
        playbackDriverIndex_ = *player;
        playbackDriverUsesRecordedRows_ = true;
    }
    (void)rebuildLapCatalog();
    strategyProtocol_ = static_cast<uint16_t>(header.protocol >= 2024 ? header.protocol : 2025);
}

bool TnrdV6Reader::hasMore() const {
    if (!indexedArchive_ || !indexedArchive_->isOpen()) return false;
    if(!playbackPrepared_)return true;
    for(const auto& lane:playbackLanes_)if(lane.rowPos<lane.rows.size()||lane.nextChunk<lane.chunks.size())return true;
    const auto& laps=indexedArchive_->laps();if(playbackLap_==0)return !laps.empty();const auto it=std::find_if(laps.begin(),laps.end(),[&](const auto&l){return (int)l.lapNumber==playbackLap_;});return it!=laps.end()&&std::next(it)!=laps.end();
}

void TnrdV6Reader::setCursor(float t) {
        playbackLap_ = indexedArchive_ ? indexedArchive_->lapAt(t) : 0;
        playbackCursor_ = t;
        if(indexedArchive_)indexedArchive_->cancelPrefetch();
        playbackPrepared_ = false;
        playbackPrefetchOutstanding_ = false;
        if (auto* v6 = indexedArchive_.get()) {
            const auto& shared = v6->sharedRecords();
            v6SharedPos_ = static_cast<size_t>(std::upper_bound(
                v6SharedOrder_.begin(), v6SharedOrder_.end(), t,
                [&](float value, size_t index) {
                    return value < v6->logicalTime(shared[index].phase, shared[index].sessionTime);
                }) - v6SharedOrder_.begin());
        }
    damageCadenceCursor_ = t;
}

void TnrdV6Reader::beginCursorPrime(float t) {
    setCursor(t);
    if (!indexedArchive_) return;

    // The directory gives the exact target lap and per-family chunk list.
    // Queue the first playback frontier before the seek-prefix query starts so
    // both operations share the archive executor and decompressed-chunk cache.
    preparePlaybackLap();
    prefetchPlaybackChunk();
}

std::vector<std::string> TnrdV6Reader::damageRowsAtCadence(
    float fromTime, float toTime, bool includeFrom,
    const CancelCheck& cancelled) const {
    std::vector<std::string> out;
    if(cancelled&&cancelled())return out;
    std::vector<TimedRaw> v4Damage;
    if (indexedArchive_) {
        std::vector<detail::V4TimedRow> rows, initial;
        std::string error;
        auto* archive = indexedArchive_.get();
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

bool TnrdV6Reader::rebuildLapCatalog() {
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
        // A display-number collision can occur after a garage restart. Keep
        // the latest interval, matching the active timeline used by lapAt().
        lapBlocks_.insert_or_assign(block.lapNum, std::move(block));
        if (lap.lapNumber == 0) continue;
        scannedLaps_.push_back({static_cast<int>(lap.lapNumber), lap.startSessionTime,
                                lap.endSessionTime, static_cast<int>(lap.lapTimeMs)});
        if (lap.lapTimeMs && (!fastestLapMs_ || static_cast<int>(lap.lapTimeMs) < fastestLapMs_)) {
            fastestLapMs_ = static_cast<int>(lap.lapTimeMs);
            fastestLapNum_ = static_cast<int>(lap.lapNumber);
        }
    }
    if (playbackDriverIndex_ >= 0) {
        const auto* v6 = indexedArchive_.get();
        const auto* driver = v6 ? v6->driverHeader(static_cast<uint8_t>(playbackDriverIndex_)) : nullptr;
        if (driver) {
            for (const auto& stint : v6TyreStints(*driver)) {
                if (stint.actual_compound <= 0) continue;
                for (auto& [lapNum, block] : lapBlocks_) {
                    if (!stint.contains(lapNum)) continue;
                    block.slimStatus.push_back({
                        "status", block.endSessionTime, 0.0,
                        stint.actual_compound, stint.visual_compound,
                    });
                }
            }
            for (auto& [lapNum, block] : lapBlocks_) {
                if (lapNum == 0 || !block.slimStatus.empty()) continue;
                if (const auto fields = v6CompoundAt(*const_cast<TnrdV6Archive*>(v6),
                        static_cast<uint8_t>(playbackDriverIndex_), block.endSessionTime))
                    block.slimStatus.push_back({
                        "status", block.endSessionTime, 0.0,
                        fields->tyre_compound, fields->visual_compound,
                    });
            }
        }
    }
    return true;
}

bool TnrdV6Reader::buildSectorDistanceMetadata() {
    for (auto& [_, block] : lapBlocks_) {
        block.sector1EndDistanceM = 0.0f;
        block.sector2EndDistanceM = 0.0f;
    }
    auto* v6 = indexedArchive_.get();
        if (v6) v6->setRequestedTypes({24});
        for (auto& [lapNum, block] : lapBlocks_) {
            if (lapNum == 0) continue;
            std::vector<detail::V4TimedRow> rows;
            std::string error;
            if (!indexedArchive_->rowsForLap(static_cast<uint32_t>(lapNum),
                                             detail::v4TypeBit(4), rows, &error)) {
                if (v6) v6->setRequestedTypes(playbackV6Types_);
                lastError_ = std::move(error);
                return false;
            }
            std::vector<LapProgressPoint> progress;
            for (const auto& row : rows) {
                if (row.rowType != 24 || row.sessionTime < block.startSessionTime ||
                    row.sessionTime > block.endSessionTime) continue;
                LapScanFields lap{};
                if (glz::read<kPartialRead>(lap, row.json)) continue;
                progress.push_back({row.sessionTime, lap.current_lap_ms, lap.lap_distance_m,
                                    lap.sector, lap.s1_ms, lap.s2_ms});
            }
            const auto [sector1, sector2] = sectorEndDistances(std::move(progress));
            block.sector1EndDistanceM = sector1;
            block.sector2EndDistanceM = sector2;
        }
    if (v6) v6->setRequestedTypes(playbackV6Types_);
    return true;
}

void TnrdV6Reader::primeCursor() {
    if (!indexedArchive_) return;
    // Seek/history extraction has normally populated time bounds and the
    // raw-chunk cache for the target. Prepare that frontier while the engine's
    // seek generation is still gated, instead of charging it to the first
    // 16 ms playback tick after the target snapshot is visible.
    (void)pullUntil(playbackCursor_);
}

std::vector<std::pair<uint8_t, std::string>> TnrdV6Reader::latestOfTypesTagged(
    float t, const std::vector<uint8_t>& types, const CancelCheck& cancelled) {
    std::vector<std::pair<uint8_t, std::string>> out;
    if(cancelled&&cancelled())return out;
    if (!indexedArchive_) return out;
        uint32_t requestedMask = 0;
        for (const auto type : types) requestedMask |= detail::v4TypeBit(type);
        if (playbackDriverIndex_ >= 0) {
            auto* v6 = indexedArchive_.get();
            if (!v6) return {};
            auto allSources = v6TypesForRowMask(requestedMask);
            std::erase_if(allSources, [&](uint8_t type) { return !v6->requestedType(type); });
            std::vector<uint8_t> multiSources;
            if (requestedMask & detail::v4TypeBit(7)) multiSources.push_back(24);
            if (requestedMask & detail::v4TypeBit(13)) multiSources.push_back(23);
            // The all-car table shows each car's tyre state; the private
            // status families describe one car and are read below for the
            // focus driver only (the playback driver's arrive as its own).
            if (requestedMask & detail::v4TypeBit(9)) multiSources.push_back(13);
            std::sort(multiSources.begin(), multiSources.end());
            multiSources.erase(std::unique(multiSources.begin(), multiSources.end()), multiSources.end());
            std::erase_if(multiSources, [&](uint8_t type) { return !v6->requestedType(type); });
            std::vector<uint8_t> selectedSources;
            std::set_difference(allSources.begin(), allSources.end(),
                                multiSources.begin(), multiSources.end(),
                                std::back_inserter(selectedSources));

            std::vector<detail::V4TimedRow> rows;
            std::string error;
            if (!selectedSources.empty()) {
                std::vector<detail::V4TimedRow> selectedRows;
                if (!indexedArchive_->latestRows(t, selectedSources, selectedRows, &error, cancelled)) {
                    if (!cancelled || !cancelled()) lastError_ = error;
                    return {};
                }
                rows.insert(rows.end(), std::make_move_iterator(selectedRows.begin()),
                            std::make_move_iterator(selectedRows.end()));
            }
            if (!multiSources.empty()) {
                std::vector<uint8_t> drivers;
                drivers.reserve(v6->driverHeaders().size());
                for (const auto& driver : v6->driverHeaders()) drivers.push_back(driver.vehicleIndex);
                std::vector<detail::V4TimedRow> multiRows;
                if (!v6->readMultiDriverLatest(t, drivers, multiSources, multiRows, &error)) {
                    lastError_ = error;
                    return {};
                }
                rows.insert(rows.end(), std::make_move_iterator(multiRows.begin()),
                            std::make_move_iterator(multiRows.end()));
            }
            if (requestedMask & detail::v4TypeBit(9)) {
                std::vector<detail::V4TimedRow> focusRows;
                if (!v6FocusStatusRows(t, focusRows)) return {};
                rows.insert(rows.end(), std::make_move_iterator(focusRows.begin()),
                            std::make_move_iterator(focusRows.end()));
            }

            const uint32_t savedOutputMask = playbackOutputRowMask_;
            playbackOutputRowMask_ = requestedMask;
            std::vector<V6ProjectedRecord> projectedSnapshot;
            uint64_t projectedOrder = 0;
            for (auto& row : rows) {
                // An edge-encoded type's newest sample carries the timestamp of
                // the change that produced it, which can be many seconds — and
                // whole laps — behind the cursor. That is the right time for a
                // history row but the wrong one for a seek snapshot: consumers
                // install these against the cursor's lap window, so a patch
                // stamped before it is discarded and the field never lands. The
                // value being restored *is* the state at the cursor, so stamp it
                // there. Sample types already sit at the cursor and are left be.
                const bool restoredState =
                    detail::stateType(static_cast<detail::V6DataType>(row.rowType));
                const float stampTime = restoredState ? t : row.sessionTime;
                std::vector<std::pair<uint8_t, std::string>> projected;
                projectV6Row(row.rowType, stampTime, row.json, projected);
                for (auto& [type, json] : projected)
                    projectedSnapshot.push_back({
                        type, stampTime, projectedOrder++, std::move(json),
                    });
            }
            std::stable_sort(projectedSnapshot.begin(), projectedSnapshot.end(),
                [](const auto& left, const auto& right) {
                    return std::tie(left.sessionTime, left.order) <
                           std::tie(right.sessionTime, right.order);
                });
            if (requestedMask & detail::v4TypeBit(2)) {
                bool hasSelectedTyreState = false;
                int selectedLap = std::max(1, v6->lapAt(t));
                float selectedTime = t;
                for (const auto& projected : projectedSnapshot) {
                    if (projected.type == 2 && scanV6StoredType(projected.json) == 13) {
                        StatusRow status{};
                        if (!glz::read<kPartialRead>(status, projected.json) &&
                            status.tyre_compound > 0) hasSelectedTyreState = true;
                    }
                }
                const auto* driver = v6->driverHeader(
                    static_cast<uint8_t>(playbackDriverIndex_));
                if (!hasSelectedTyreState && driver) {
                    const auto stints = v6TyreStints(*driver);
                    const auto* currentStint = tyreStintForLap(stints, selectedLap);
                    if (currentStint && currentStint->actual_compound > 0) {
                        std::string status =
                            "{\"type\":\"status\",\"_v6_type\":13,\"player_idx\":" +
                            std::to_string(playbackDriverIndex_) +
                            ",\"session_time\":" + std::to_string(selectedTime) +
                            ",\"tyre_compound\":" + std::to_string(currentStint->actual_compound) +
                            ",\"visual_compound\":" + std::to_string(currentStint->visual_compound) +
                            ",\"tyre_age_laps\":" +
                            std::to_string(std::max(0, selectedLap - currentStint->start_lap)) + "}";
                        projectedSnapshot.push_back({
                            2, selectedTime, projectedOrder++, std::move(status),
                        });
                    }
                }
                std::stable_sort(projectedSnapshot.begin(), projectedSnapshot.end(),
                    [](const auto& left, const auto& right) {
                        return std::tie(left.sessionTime, left.order) <
                               std::tie(right.sessionTime, right.order);
                    });
            }
            if (requestedMask & detail::v4TypeBit(9)) {
                std::unordered_set<int> driversWithTyreState;
                for (const auto& projected : projectedSnapshot) {
                    if (projected.type != 9 || scanV6StoredType(projected.json) != 13) continue;
                    AllStatusRow status{};
                    if (glz::read<kPartialRead>(status, projected.json)) continue;
                    for (const auto& car : status.cars)
                        if (car.tyre_compound > 0) driversWithTyreState.insert(car.idx);
                }
                std::vector<V6ProjectedRecord> summaryPatches;
                for (const auto& projected : projectedSnapshot) {
                    if (projected.type != 7) continue;
                    TimingRow timing{};
                    if (glz::read<kPartialRead>(timing, projected.json)) continue;
                    for (const auto& timingCar : timing.cars) {
                        if (driversWithTyreState.contains(timingCar.idx)) continue;
                        const auto* driver = timingCar.idx >= 0
                            ? v6->driverHeader(static_cast<uint8_t>(timingCar.idx))
                            : nullptr;
                        if (!driver) continue;
                        const auto stints = v6TyreStints(*driver);
                        const auto* currentStint = tyreStintForLap(stints, timingCar.lap_num);
                        if (!currentStint || currentStint->actual_compound <= 0) continue;
                        AllStatusRow status{};
                        status.session_time = projected.sessionTime;
                        status.cars.push_back({});
                        auto& statusCar = status.cars.back();
                        statusCar.idx = timingCar.idx;
                        statusCar.tyre_compound = currentStint->actual_compound;
                        statusCar.visual_compound = currentStint->visual_compound;
                        statusCar.tyre_age_laps = std::max(0, timingCar.lap_num - currentStint->start_lap);
                        std::string json;
                        if (!glz::write_json(status, json)) {
                            tagV6StoredType(json, 13);
                            summaryPatches.push_back({
                                9, status.session_time, projectedOrder++, std::move(json),
                            });
                            driversWithTyreState.insert(timingCar.idx);
                        }
                    }
                }
                projectedSnapshot.insert(projectedSnapshot.end(),
                    std::make_move_iterator(summaryPatches.begin()),
                    std::make_move_iterator(summaryPatches.end()));
                std::stable_sort(projectedSnapshot.begin(), projectedSnapshot.end(),
                    [](const auto& left, const auto& right) {
                        return std::tie(left.sessionTime, left.order) <
                               std::tie(right.sessionTime, right.order);
                    });
            }
            if (!sparseV6Playback_) {
                V6ProjectionAssembler assembler;
                for (auto& row : projectedSnapshot)
                    assembler.add(row.type, row.sessionTime, std::move(row.json));
                projectedSnapshot = assembler.take();
                std::array<size_t, 16> latestProjected{};
                latestProjected.fill(projectedSnapshot.size());
                for (size_t index = 0; index < projectedSnapshot.size(); ++index) {
                    const uint8_t type = projectedSnapshot[index].type;
                    if (type < latestProjected.size()) latestProjected[type] = index;
                }
                std::vector<V6ProjectedRecord> latestRows;
                latestRows.reserve(projectedSnapshot.size());
                for (size_t index = 0; index < projectedSnapshot.size(); ++index) {
                    auto& row = projectedSnapshot[index];
                    if (row.type < latestProjected.size() && latestProjected[row.type] == index)
                        latestRows.push_back(std::move(row));
                }
                projectedSnapshot = std::move(latestRows);
                for (const auto& row : projectedSnapshot)
                    if (row.type == 7 || row.type == 9 || row.type == 13)
                        v6ProjectionState_[row.type] = row.json;
            }
            for (auto& row : projectedSnapshot)
                out.emplace_back(row.type, std::move(row.json));
            playbackOutputRowMask_ = savedOutputMask;

            std::array<const detail::V6SharedRecord*, 16> latestShared{};
            std::array<float, 16> latestSharedTime{};
            latestSharedTime.fill(-std::numeric_limits<float>::infinity());
            for (const auto& record : v6->sharedRecords()) {
                if (record.phase != detail::V6Phase::Race) continue;
                const uint8_t type = scanType(record.json.data(), static_cast<int>(record.json.size()));
                if (!(requestedMask & detail::v4TypeBit(type))) continue;
                const float time = v6->logicalTime(record.phase, record.sessionTime);
                if (time <= t && time >= latestSharedTime[type]) {
                    latestShared[type] = &record;
                    latestSharedTime[type] = time;
                }
            }
            // Preserve V5's immediately populated session/roster panels when
            // their first packet is fractionally newer than the first playable
            // per-driver sample.
            for (const uint8_t fallbackType : {uint8_t{5}, uint8_t{8}}) {
                if (!(requestedMask & detail::v4TypeBit(fallbackType)) || latestShared[fallbackType])
                    continue;
                float earliest = std::numeric_limits<float>::infinity();
                for (const auto& record : v6->sharedRecords()) {
                    if (record.phase != detail::V6Phase::Race) continue;
                    if (scanType(record.json.data(), static_cast<int>(record.json.size())) != fallbackType)
                        continue;
                    const float time = v6->logicalTime(record.phase, record.sessionTime);
                    if (time >= earliest) continue;
                    earliest = time;
                    latestShared[fallbackType] = &record;
                    latestSharedTime[fallbackType] = time;
                }
            }
            for (uint8_t type : types) if (type < latestShared.size() && latestShared[type]) {
                std::string json = latestShared[type]->json;
                setSessionTime(json, latestSharedTime[type]);
                out.emplace_back(type, std::move(json));
            }
            std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
                return scanSessionTime(a.second.data(), static_cast<int>(a.second.size())) <
                       scanSessionTime(b.second.data(), static_cast<int>(b.second.size()));
            });
            return out;
        }
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

std::vector<std::string> TnrdV6Reader::readRange(float fromTime, float toTime) {
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

bool TnrdV6Reader::forEachIndexedRange(
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

bool TnrdV6Reader::forEachStrategyRow(
    float fromTime, float toTime, bool includeFrom,
    const std::function<void(float, std::string_view)>& callback,
    const CancelCheck& cancelled) {
    if(cancelled&&cancelled())return false;
    if (toTime < fromTime) return true;
    if (!indexedArchive_) return false;
            // V6 keeps a driver's state as per-field families, not as the
            // composite rows the strategy reducer parses: handing them over raw
            // leaves it without lap, status and damage, so it never leaves its
            // waiting state. Project them the way the playback cursor does, and
            // re-merge each timestamp's families into one row per legacy type —
            // the reducer replaces its stored row wholesale, so a sparse patch
            // would blank the fields it does not carry. Shared V6 records and
            // every V5 row are already composite and pass straight through.
            V6ProjectionAssembler assembler;
            bool assembling = false;
            float assembledTime = 0.0f;
            const auto drain = [&] {
                assembling = false;
                for (const auto& record : assembler.take())
                    callback(record.sessionTime, record.json);
            };
            // Read in bounded slices: one read of a whole late-race range holds
            // every car's decoded rows at once, which exhausts a phone's memory.
            // A timestamp falls in exactly one slice, so a timestamp's families
            // are still assembled together.
            const bool ok = forEachIndexedRange(fromTime, toTime, kStrategyDependencyMask,
                [&](const detail::V4TimedRow& row) {
                if (!includeFrom && row.sessionTime <= fromTime) return true;
                // Range reads return family rows untagged: only the playback
                // cursor adds the _v6_type marker. A family row is prefixed with
                // its driver_idx and its rowType is the V6 family; a shared
                // record is a plain legacy row whose rowType is its legacy type.
                const bool familyRow =
                    std::string_view(row.json).substr(0, 14) == "{\"driver_idx\":";
                const uint8_t storedType = familyRow ? row.rowType : 0;
                if (storedType != 0) {
                    std::vector<std::pair<uint8_t, std::string>> projected;
                    projectV6Row(storedType, row.sessionTime, row.json, projected,
                                 -1, kStrategyDependencyMask);
                    if (projected.empty()) return true;
                    if (assembling && row.sessionTime != assembledTime) drain();
                    assembling = true;
                    assembledTime = row.sessionTime;
                    for (auto& [type, json] : projected)
                        assembler.add(type, row.sessionTime, std::move(json));
                    return true;
                }
                if (assembling) drain();
                callback(row.sessionTime, row.json);
                return true;
            }, cancelled);
            if (!ok) return false;
            if (assembling) drain();
            return true;
}

void TnrdV6Reader::playbackV6TypesChanged(float cursorTime) {
    if (!indexedArchive_) return;
    indexedArchive_->setRequestedTypes(playbackV6Types_);
    setCursor(cursorTime);
}

void TnrdV6Reader::setPlaybackDriver(int driverIndex, bool useRecordedRows, float cursorTime) {
    (void)useRecordedRows;
    const int next = driverIndex >= 0 ? driverIndex : recordedDriverIndex_;
    const bool nextUsesRecordedRows = next >= 0;
    if (playbackDriverIndex_ == next &&
        playbackDriverUsesRecordedRows_ == nextUsesRecordedRows) return;
    auto* v6 = indexedArchive_.get();
    if (!v6 || next < 0 || !v6->driverHeader(static_cast<uint8_t>(next))) return;
    playbackDriverIndex_ = next;
    playbackDriverUsesRecordedRows_ = nextUsesRecordedRows;
    v6->setPlaybackDriver(static_cast<uint8_t>(next));
    startTime_ = v6->startTime();
    totalTime_ = v6->totalTime();
    (void)rebuildLapCatalog();
    strategyCheckpoints_.clear();
    (void)buildSectorDistanceMetadata();
    playbackRowMask_ = expandedPlaybackMask(playbackOutputRowMask_);
    setCursor(cursorTime);
}

bool TnrdV6Reader::v6FocusStatusRows(float t, std::vector<V4TimedRow>& out) {
    out.clear();
    auto* v6 = indexedArchive_.get();
    if (!v6) return true;
    const int focus = v6->focusDriver();
    if (focus < 0 || focus == playbackDriverIndex_) return true;
    std::vector<uint8_t> types;
    for (const uint8_t type : {7, 15, 16, 17, 18, 19, 20})
        if (v6->requestedType(type)) types.push_back(type);
    if (types.empty()) return true;
    std::string error;
    if (!v6->readMultiDriverLatest(t, {static_cast<uint8_t>(focus)}, types, out, &error)) {
        lastError_ = error;
        return false;
    }
    return true;
}

void TnrdV6Reader::setV6FocusDriver(int driverIndex, float cursorTime) {
    auto* v6 = indexedArchive_.get();
    if (!v6 || v6->focusDriver() == driverIndex) return;
    v6->setFocusDriver(driverIndex);
    // The focus driver's private families join or leave the playback lanes.
    setCursor(cursorTime);
}

std::vector<std::string> TnrdV6Reader::v6FocusStatusSnapshot(float t) {
    std::vector<std::string> out;
    std::vector<V4TimedRow> rows;
    if (!v6FocusStatusRows(t, rows)) return out;
    const uint32_t savedOutputMask = playbackOutputRowMask_;
    playbackOutputRowMask_ = detail::v4TypeBit(9);
    for (const auto& row : rows) {
        // Edge-encoded state is restored at the cursor, as a seek does.
        const float stampTime = detail::stateType(static_cast<detail::V6DataType>(row.rowType))
            ? t : row.sessionTime;
        std::vector<std::pair<uint8_t, std::string>> projected;
        projectV6Row(row.rowType, stampTime, row.json, projected);
        for (auto& [type, json] : projected)
            if (type == 9) out.push_back(std::move(json));
    }
    playbackOutputRowMask_ = savedOutputMask;
    return out;
}

bool TnrdV6Reader::v6FastestLapAt(float t, FastestLapRow& out) const {
    const auto* v6 = indexedArchive_.get();
    if (!v6) return false;
    if (!v6FastestLapChanges_) {
        struct Completed { float end; int car; int lapMs; };
        std::vector<Completed> laps;
        for (const auto& driver : v6->driverHeaders())
            for (const auto& lap : v6->driverLapSummaries(driver.vehicleIndex))
                if (lap.phase == detail::V6Phase::Race && lap.isCompleted && lap.isValid &&
                    lap.lapTimeMs > 0 && lap.lapNumber > 0)
                    laps.push_back({v6->logicalTime(lap.phase, lap.endSessionTime),
                                    static_cast<int>(driver.vehicleIndex),
                                    static_cast<int>(lap.lapTimeMs)});
        std::stable_sort(laps.begin(), laps.end(),
            [](const Completed& a, const Completed& b) { return a.end < b.end; });
        auto& changes = v6FastestLapChanges_.emplace();
        for (const auto& lap : laps)
            if (changes.empty() || lap.lapMs < changes.back().lapMs)
                changes.push_back({lap.end, lap.car, lap.lapMs});
    }
    const auto& changes = *v6FastestLapChanges_;
    const auto after = std::upper_bound(changes.begin(), changes.end(), t,
        [](float time, const V6FastestLapChange& change) { return time < change.time; });
    out = {};
    out.car_idx = -1;
    if (after != changes.begin()) {
        const auto& holder = *std::prev(after);
        out.car_idx = holder.car;
        out.lap_time_s = static_cast<float>(holder.lapMs) / 1000.0f;
    }
    return true;
}

float TnrdV6Reader::v6NextLapEndAfter(float t) const {
    const auto* v6 = indexedArchive_.get();
    if (!v6) return std::numeric_limits<float>::infinity();
    if (!v6LapEnds_) {
        auto& ends = v6LapEnds_.emplace();
        for (const auto& driver : v6->driverHeaders())
            for (const auto& lap : v6->driverLapSummaries(driver.vehicleIndex))
                if (lap.phase == detail::V6Phase::Race && lap.isCompleted && lap.lapTimeMs > 0 &&
                    lap.lapNumber > 0)
                    ends.push_back(v6->logicalTime(lap.phase, lap.endSessionTime));
        std::sort(ends.begin(), ends.end());
    }
    const auto next = std::upper_bound(v6LapEnds_->begin(), v6LapEnds_->end(), t);
    return next == v6LapEnds_->end() ? std::numeric_limits<float>::infinity() : *next;
}

// Read on first use: only the driver whose Tyres page is shown needs one, and
// only for rows recorded before the parser filled avg_wear_per_lap. Tyre state
// is edge-encoded, so the opening minutes almost always hold the first sets.
const std::vector<TyreSet>& TnrdV6Reader::firstV6TyreSets(int car) const {
    auto& first = v6FirstTyreSets_[static_cast<size_t>(car)];
    if (first) return *first;
    first.emplace();
    auto* v6 = indexedArchive_.get();
    if (!v6) return *first;
    std::vector<detail::V6TimedRow> rows;
    std::string ignored;
    for (const float to : {startTime_ + 600.0f, std::numeric_limits<float>::max()}) {
        if (!v6->readDriverRangeTypes(static_cast<uint8_t>(car), -std::numeric_limits<float>::max(),
                                      to, {13}, rows, &ignored)) break;
        for (const auto& row : rows) {
            if (row.json.find("\"sets\":") == std::string::npos) continue;
            TyreSetsRow sets;
            if (!glz::read<kPartialRead>(sets, row.json) && !sets.sets.empty()) {
                *first = std::move(sets.sets);
                return *first;
            }
        }
    }
    return *first;
}

DriverLapHistoryRow TnrdV6Reader::driverLapHistory(int driver, float throughTime) const {
    DriverLapHistoryRow out;
    out.car_idx = driver;
    const auto* v6 = indexedArchive_.get();
    if (!v6 || driver < 0 || driver >= 24) return out;
    // A garage restart can reuse a lap number; the latest interval wins, as in
    // the lap catalog.
    std::map<uint32_t, SessionHistoryLap> byLap;
    for (const auto& lap : v6->driverLapSummaries(static_cast<uint8_t>(driver))) {
        if (lap.phase != detail::V6Phase::Race || !lap.isCompleted || lap.lapTimeMs == 0 ||
            lap.lapNumber == 0 || v6->logicalTime(lap.phase, lap.endSessionTime) > throughTime)
            continue;
        byLap[lap.lapNumber] = {static_cast<int>(lap.lapNumber), static_cast<int>(lap.lapTimeMs),
            static_cast<int>(lap.s1Ms), static_cast<int>(lap.s2Ms), static_cast<int>(lap.s3Ms),
            lap.isValid, lap.s1Ms > 0, lap.s2Ms > 0, lap.s3Ms > 0};
    }
    for (auto& [_, lap] : byLap) out.laps.push_back(lap);
    // The header holds the whole session's stints; the lap in progress at the
    // cursor decides which of them was in use then.
    if (const auto* header = v6->driverHeader(static_cast<uint8_t>(driver))) {
        const int currentLap = byLap.empty() ? 1 : static_cast<int>(byLap.rbegin()->first) + 1;
        out.stint_start_lap = tyreStintStartLap(v6TyreStints(*header), currentLap);
    }
    return out;
}

LapMeasuresRow TnrdV6Reader::lapMeasures(int driver, float throughTime, bool hasMguh) const {
    LapMeasuresRow out;
    out.car_idx = driver;
    auto* v6 = indexedArchive_.get();
    if (!v6 || driver < 0 || driver >= 24) return out;
    auto& measured = v6LapMeasures_[static_cast<size_t>(driver)];
    if (!measured) {
        measured.emplace();
        // The laps driverLapHistory lists; a reused lap number keeps its latest attempt.
        std::map<uint32_t, detail::V6LapSummary> byLap;
        for (const auto& lap : v6->driverLapSummaries(static_cast<uint8_t>(driver)))
            if (lap.phase == detail::V6Phase::Race && lap.isCompleted && lap.lapTimeMs != 0 &&
                lap.lapNumber != 0)
                byLap[lap.lapNumber] = lap;
        // Each line is read once: a lap usually starts where the one before ended.
        std::map<float, LapLineReading> lines;
        for (const auto& [_, lap] : byLap) {
            lines[lap.startSessionTime];
            lines[lap.endSessionTime];
        }
        static const std::vector<uint8_t> types{
            static_cast<uint8_t>(detail::V6DataType::Fuel),
            static_cast<uint8_t>(detail::V6DataType::ERSHarvest),
            static_cast<uint8_t>(detail::V6DataType::ERSDeployment)};
        std::vector<detail::V6TimedRow> rows;
        std::vector<LapLineSample> samples;
        std::string ignored;
        for (auto& [line, reading] : lines) {
            if (!v6->readDriverRangeTypes(static_cast<uint8_t>(driver), line - kLapLineWindowS,
                                          line + kLapLineWindowS, types, rows, &ignored))
                continue;
            // Each type is its own patch: a row fills only its own fields.
            samples.clear();
            samples.reserve(rows.size());
            for (const auto& row : rows)
                samples.push_back({row.sessionTime,
                                   v6RowNumber(row.json, "\"ers_deployed_j\":"),
                                   v6RowNumber(row.json, "\"ers_harvested_mguk_j\":"),
                                   v6RowNumber(row.json, "\"ers_harvested_mguh_j\":"),
                                   v6RowNumber(row.json, "\"fuel_kg\":")});
            reading = readLapLine(samples, line);
        }
        for (const auto& [lapNumber, lap] : byLap)
            (*measured)[static_cast<int>(lapNumber)] = {
                v6->logicalTime(lap.phase, lap.endSessionTime),
                lapTotals(lap.startSessionTime, lap.endSessionTime,
                          lines[lap.startSessionTime], lines[lap.endSessionTime])};
    }
    for (const auto& [lapNumber, lap] : *measured)
        if (lap.logicalEnd <= throughTime) out.laps.push_back(lapMeasure(lapNumber, lap.totals, hasMguh));
    return out;
}

void TnrdV6Reader::projectV6Row(
    uint8_t sourceType, float sessionTime, std::string_view json,
    std::vector<std::pair<uint8_t, std::string>>& out,
    int selectedDriverOverride, uint32_t outputMaskOverride) const {
    const uint32_t outputMask = outputMaskOverride != 0
        ? outputMaskOverride : playbackOutputRowMask_;
    const auto wants = [&](uint8_t type) {
        return (outputMask & detail::v4TypeBit(type)) != 0;
    };
    if (sourceType == 0 || sourceType > 24) return;
    const auto wrap = [&](std::string_view type, int driver, bool asCar) {
        if (json.empty() || json.front() != '{') return std::string{};
        std::string payload(json);
        const auto marker = payload.find("\"_v6_type\":");
        if (marker != std::string::npos) {
            size_t end = marker + 11; while (end < payload.size() && payload[end] != ',' && payload[end] != '}') ++end;
            if (end < payload.size() && payload[end] == ',') ++end;
            payload.erase(marker, end - marker);
        }
        const auto timeKey = payload.find("\"session_time\":");
        if (timeKey != std::string::npos) {
            const size_t begin = timeKey + 15;
            size_t end = begin; while (end < payload.size() && payload[end] != ',' && payload[end] != '}') ++end;
            char buffer[32]; std::snprintf(buffer, sizeof(buffer), "%.9g", sessionTime);
            payload.replace(begin, end - begin, buffer);
        }
        const int selectedDriver = selectedDriverOverride >= 0
            ? selectedDriverOverride
            : (playbackDriverIndex_ >= 0 ? playbackDriverIndex_ : recordedDriverIndex_);
        std::string value = "{\"type\":\"" + std::string(type) + "\",\"_v6_type\":" +
            std::to_string(sourceType) + ",\"player_idx\":" +
            std::to_string(selectedDriver) + ',';
        if (asCar) value += "\"session_time\":" + std::to_string(sessionTime) + ',';
        if (asCar) value += "\"cars\":[{\"idx\":" + std::to_string(driver) + ',';
        value.append(std::string_view(payload).substr(1, payload.size() - 2));
        if (asCar) value += "}]";
        value += '}'; return value;
    };
    // Tyre sets belong to a car rather than to the player, so the projected row
    // names it as car_idx instead of player_idx.
    const auto tyreSetsRow = [&](int carIdx) {
        std::string row = wrap("tyre_sets", carIdx, false);
        const auto playerKey = row.find("\"player_idx\":");
        if (playerKey != std::string::npos) {
            const auto valueEnd = row.find(',', playerKey);
            if (valueEnd != std::string::npos)
                row.replace(playerKey, valueEnd - playerKey,
                            "\"car_idx\":" + std::to_string(carIdx));
        }
        if (carIdx >= 0 && carIdx < static_cast<int>(v6FirstTyreSets_.size()) &&
            json.find("\"avg_wear_per_lap\"") == std::string_view::npos)
            fillRecordedTyreSetWear(row, json, firstV6TyreSets(carIdx));
        return row;
    };
    const int selectedDriver = selectedDriverOverride >= 0
        ? selectedDriverOverride
        : (playbackDriverIndex_ >= 0 ? playbackDriverIndex_ : recordedDriverIndex_);
    const int driver = scanV6Driver(json, selectedDriver);
    const bool selected = driver == selectedDriver;
    if (sourceType == 7 && json.find("\"drs_allowed\"") != std::string_view::npos) {
        if (selected && wants(2)) out.emplace_back(2, wrap("status", driver, false));
        if (wants(9)) out.emplace_back(9, wrap("all_status", driver, true));
    }
    else if (sourceType <= 11 && selected && wants(1)) out.emplace_back(1, wrap("telemetry", driver, false));
    else if ((sourceType == 12 || sourceType == 14) && selected && wants(3)) out.emplace_back(3, wrap("damage", driver, false));
    else if (sourceType == 13 && json.find("\"sets\":") != std::string_view::npos) {
        if (selected && wants(10)) out.emplace_back(10, tyreSetsRow(driver));
    }
    else if (sourceType == 13 || (sourceType >= 15 && sourceType <= 20)) {
        // Losing access is a state change like any other and has to reach every
        // consumer still showing the old public value. The availability sample
        // carries no field names, so the guards below cannot look for them.
        const bool withdrawn =
            json.find("\"available\":false") != std::string_view::npos;
        if (selected && wants(2)) out.emplace_back(2, wrap("status", driver, false));
        if (wants(9) && (sourceType != 13 || withdrawn ||
                         json.find("\"tyre_compound\"") != std::string_view::npos))
            out.emplace_back(9, wrap("all_status", driver, true));
        // Tyre sets share this type, so a withdrawal arrives without a "sets"
        // array and would otherwise leave the last public sets on screen.
        if (sourceType == 13 && withdrawn && selected && wants(10))
            out.emplace_back(10, tyreSetsRow(driver));
    }
    else if (sourceType == 21 && selected && wants(11)) out.emplace_back(11, wrap("motion", driver, false));
    else if (sourceType == 22 && selected && wants(12)) out.emplace_back(12, wrap("motion_ex", driver, false));
    else if (sourceType == 23 && wants(13)) out.emplace_back(13, wrap("positions", driver, true));
    else if (sourceType == 24) {
        if (selected && wants(4)) out.emplace_back(4, wrap("lap", driver, false));
        if (wants(7)) out.emplace_back(7, wrap("timing", driver, true));
    }
}

std::string TnrdV6Reader::phoneChartLapJson(int lapNum, const std::vector<std::string>& channels) const {
    // The sparse V6 rows of the recorded driver's laps: what the phone plays
    // until another car is chosen.
    if (!sparseV6Playback_ ||
        (playbackDriverIndex_ >= 0 && playbackDriverIndex_ != recordedDriverIndex_)) return {};
    auto* v6 = indexedArchive_.get();
    if (!v6) return {};
    const auto block = lapBlocks_.find(lapNum);
    if (block == lapBlocks_.end()) return {};
    const LapBlock& b = block->second;
    // The families asked for and lap progress; never positions.
    const uint32_t rowMask = pairLapDataRowMask(channels) & ~detail::v4TypeBit(13);
    const auto wants = [&](uint8_t type) { return (rowMask & detail::v4TypeBit(type)) != 0; };
    detail::V6LapFields rows;
    std::string error;
    if (!v6->lapFields(static_cast<uint32_t>(lapNum), v6TypesForRowMask(rowMask),
                       b.startSessionTime, b.endSessionTime, rows, &error)) return {};

    // Each family's asked-for fields, by the lap's field name index.
    constexpr std::string_view kFamilies[] = {"telemetry", "status", "damage", "motion", "motion_ex"};
    constexpr size_t kFamilyCount = std::size(kFamilies);
    std::vector<uint8_t> askedBy(rows.names.size(), 0);  // bit f: family f asks for it
    for (const auto& channel : channels) {
        const size_t dot = channel.find('.');
        if (dot == std::string::npos) continue;
        const std::string_view family(channel.data(), dot);
        const std::string_view field(channel.data() + dot + 1, channel.size() - dot - 1);
        for (size_t f = 0; f < kFamilyCount; ++f) {
            if (kFamilies[f] != family) continue;
            for (size_t n = 0; n < rows.names.size(); ++n)
                if (rows.names[n] == field) askedBy[n] |= static_cast<uint8_t>(1u << f);
        }
    }
    const auto nameId = [&](std::string_view name) {
        for (size_t n = 0; n < rows.names.size(); ++n) if (rows.names[n] == name) return static_cast<int>(n);
        return -1;
    };
    const int drsAllowed = nameId("drs_allowed");
    const int currentLapMs = nameId("current_lap_ms"), lapDistance = nameId("lap_distance_m"),
              sector = nameId("sector"), s1 = nameId("s1_ms"), s2 = nameId("s2_ms");

    // getLapDataMessage() renders reals with "%.9g" and the reply reads them
    // back; reading them the same way keeps both replies identical.
    const auto asRead = [](double value) {
        char text[32];
        const auto written = std::to_chars(text, text + sizeof(text) - 1, value, std::chars_format::general, 9);
        *written.ptr = '\0';
        return std::strtod(text, nullptr);
    };
    // Rows of several types share a time: read each time once.
    float lastTime = std::numeric_limits<float>::quiet_NaN();
    double lastTimeRead = 0.0;
    PairLapSamples lap;
    lap.lapNum = b.lapNum;
    lap.startSessionTime = b.startSessionTime;
    lap.endSessionTime = b.endSessionTime;
    std::array<std::vector<PairLapSample>*, kFamilyCount> samples{};
    for (const auto& row : rows.rows) {
        const auto first = rows.fields.begin() + row.first;
        const auto last = first + row.count;
        // projectV6Row()'s routing for the selected driver.
        const uint8_t type = row.type;
        int family = -1;
        if (type == 7 && drsAllowed >= 0 &&
            std::any_of(first, last, [&](const auto& field) { return static_cast<int>(field.name) == drsAllowed; })) {
            if (wants(2)) family = 1;
        }
        else if (type <= 11) { if (wants(1)) family = 0; }
        else if (type == 12 || type == 14) { if (wants(3)) family = 2; }
        else if (type == 13 && row.sets) {}
        else if (type == 13 || (type >= 15 && type <= 20)) { if (wants(2)) family = 1; }
        else if (type == 21) { if (wants(11)) family = 3; }
        else if (type == 22) { if (wants(12)) family = 4; }
        else if (type == 24 && wants(4)) {
            LapProgressPoint point{row.time};
            for (auto field = first; field != last; ++field) {
                const int name = static_cast<int>(field->name);
                if (name == currentLapMs) point.current_lap_ms = static_cast<int>(field->value);
                else if (name == lapDistance) point.lap_distance_m = static_cast<float>(field->value);
                else if (name == sector) point.sector = static_cast<int>(field->value);
                else if (name == s1) point.s1_ms = static_cast<int>(field->value);
                else if (name == s2) point.s2_ms = static_cast<int>(field->value);
            }
            lap.progress.push_back(point);
        }
        // A non-finite real makes the rendered row unreadable, so the reply skips it.
        if (family < 0 || !row.finite) continue;
        // Every row of a family counts for its times; only asked-for fields are kept.
        if (row.time != lastTime) {
            lastTime = row.time;
            lastTimeRead = asRead(row.time);
        }
        PairLapSample sample{lastTimeRead, type, row.unavailable, {}};
        const uint8_t bit = static_cast<uint8_t>(1u << family);
        for (auto field = first; field != last; ++field)
            if (askedBy[field->name] & bit)
                sample.fields.emplace_back(rows.names[field->name], field->real ? asRead(field->value) : field->value);
        auto*& list = samples[static_cast<size_t>(family)];
        if (!list) list = &lap.families[std::string(kFamilies[family])];
        list->push_back(std::move(sample));
    }
    return pairLapDataJson(lap, channels);
}

bool TnrdV6Reader::currentLapAt(float t, float& startOut, int& numOut) const {
    if (!indexedArchive_) return false;
    if (playbackDriverIndex_ >= 0 && !playbackDriverUsesRecordedRows_) {
            // V6's persisted lap directory follows the recorded player. For a
            // selected driver, derive their own lap origin from the latest
            // all-car LapData sample instead: currentLapTimeInMS is measured
            // from that driver's most recent timing-line crossing.
            std::vector<detail::V4TimedRow> rows;
            std::string error;
            if (indexedArchive_.get()->latestRows(
                    t, {7}, rows, &error)) {
                for (auto row = rows.rbegin(); row != rows.rend(); ++row) {
                    TimingRow timing{};
                    if (glz::read<kPartialRead>(timing, row->json)) continue;
                    const auto car = std::find_if(timing.cars.begin(), timing.cars.end(),
                        [&](const auto& value) { return value.idx == playbackDriverIndex_; });
                    if (car == timing.cars.end()) continue;
                    numOut = car->lap_num;
                    startOut = std::clamp(
                        row->sessionTime - std::max(0, car->current_lap_ms) / 1000.0f,
                        startTime_, t);
                    return true;
                }
            }
            // A driver may not yet have a valid array entry at the very start
            // of a recording. Fall back to the recorded-player directory so a
            // seek still produces a bounded prefix instead of an empty one.
        }
        numOut = indexedArchive_->lapAt(t);
        for (const auto& l : indexedArchive_->laps()) {
            if (static_cast<int>(l.lapNumber) == numOut) {
                startOut = l.startSessionTime;
                return true;
            }
        }
        return false;
}

std::string TnrdV6Reader::driverRestrictionMessage(float cursorTime) const {
    
    const auto* v6 = indexedArchive_.get();
    const int driver = playbackDriverIndex_ >= 0 ? playbackDriverIndex_ : recordedDriverIndex_;
    if (!v6 || driver < 0) return {};
    const auto index = static_cast<uint8_t>(driver);
    if (!v6->driverHeader(index)) return {};
    DriverRestrictionRow row;
    row.driverIndex = driver;
    row.restricted = !v6->privateDataAvailableAt(index, cursorTime);
    row.known = !row.restricted ||
        v6->telemetrySettingAt(index, cursorTime) != detail::TelemetrySetting::Unknown;
    std::string out;
    if (glz::write_json(row, out)) return {};
    return out;
}

void TnrdV6Reader::addAnalysisDrivers(PlaybackLapBlocksRow& msg) const {
    const auto* v6 = indexedArchive_.get();
    if (v6) msg.tnrdRevision = v6->revision();
    msg.playbackDriverIndex = playbackDriverIndex_;
        if (v6) {
            msg.analysisDrivers.reserve(v6->driverHeaders().size());
            for (const auto& driver : v6->driverHeaders()) {
                AnalysisDriverLapCatalog catalog;
                catalog.driverIndex = driver.vehicleIndex;
                catalog.driverName = driver.driverName;
                catalog.isPlayer = driver.isPlayer;
                catalog.raceNumber = driver.raceNumber;
                catalog.teamId = driver.teamId;
                catalog.restricted = !driver.isPlayer &&
                    v6->telemetrySettingAt(driver.vehicleIndex, std::numeric_limits<float>::max()) !=
                        detail::TelemetrySetting::Public;
                std::map<int, detail::V6LapSummary> lapsByNumber;
                for (const auto& lap : v6->driverLapSummaries(driver.vehicleIndex)) {
                    if (lap.phase != detail::V6Phase::Race || lap.lapNumber == 0) continue;
                    lapsByNumber.insert_or_assign(static_cast<int>(lap.lapNumber), lap);
                }
                int fastestMs = 0;
                for (const auto& [lapNumber, lap] : lapsByNumber) {
                    LapBlockMeta block;
                    block.lapNum = lapNumber;
                    block.startSessionTime = lap.startSessionTime;
                    block.endSessionTime = lap.endSessionTime;
                    catalog.blocks.push_back(std::move(block));
                    catalog.laps.push_back({lapNumber, static_cast<int>(lap.lapTimeMs)});
                    if (lap.lapTimeMs > 0 && (!fastestMs || static_cast<int>(lap.lapTimeMs) < fastestMs)) {
                        fastestMs = static_cast<int>(lap.lapTimeMs);
                        catalog.fastestLapNum = lapNumber;
                    }
                }
                for (const auto& stint : v6TyreStints(driver)) {
                    if (stint.actual_compound <= 0) continue;
                    for (auto& block : catalog.blocks) {
                        if (!stint.contains(block.lapNum)) continue;
                        block.statusHistory.push_back({
                            "status", block.endSessionTime, 0.0,
                            stint.actual_compound, stint.visual_compound,
                        });
                    }
                }
                for (auto& block : catalog.blocks) {
                    if (!block.statusHistory.empty()) continue;
                    if (const auto fields = v6CompoundAt(*const_cast<detail::TnrdV6Archive*>(v6),
                            driver.vehicleIndex, block.endSessionTime))
                        block.statusHistory.push_back({
                            "status", block.endSessionTime, 0.0,
                            fields->tyre_compound, fields->visual_compound,
                        });
                }
                msg.analysisDrivers.push_back(std::move(catalog));
            }
        }
}

std::string TnrdV6Reader::getLapDataMessage(int lapNum, uint32_t rowTypeMask,
                                            int driverIndex) const {
    if (driverIndex >= 0 && driverIndex != playbackDriverIndex_) {
        auto* v6 = indexedArchive_.get();
        if (!v6 || !v6->driverHeader(static_cast<uint8_t>(driverIndex))) return {};
        std::optional<detail::V6LapSummary> selectedLap;
        for (const auto& lap : v6->driverLapSummaries(static_cast<uint8_t>(driverIndex))) {
            if (lap.phase == detail::V6Phase::Race &&
                static_cast<int>(lap.lapNumber) == lapNum) selectedLap = lap;
        }
        if (!selectedLap) return {};

        PlaybackLapDataRow msg;
        msg.lapNum = lapNum;
        msg.startSessionTime = selectedLap->startSessionTime;
        msg.endSessionTime = selectedLap->endSessionTime;
        msg.rowTypeMask = rowTypeMask;
        const auto selectedTypes = v6TypesForRowMask(rowTypeMask);
        std::vector<detail::V4TimedRow> rows;
        std::string error;
        if (!v6->readDriverLapTypes(static_cast<uint8_t>(driverIndex), selectedLap->lapId,
                                    selectedTypes, rows, &error)) return {};
        std::vector<V6ProjectedRecord> outputRows;
        uint64_t outputOrder = 0;
        for (const auto& row : rows) {
            if (row.sessionTime < msg.startSessionTime || row.sessionTime > msg.endSessionTime) continue;
            std::vector<std::pair<uint8_t, std::string>> projected;
            projectV6Row(row.rowType, row.sessionTime, row.json, projected,
                         driverIndex, rowTypeMask);
            for (auto& [type, json] : projected)
                outputRows.push_back({type, row.sessionTime, outputOrder++, std::move(json)});
        }
        if (!sparseV6Playback_) {
            V6ProjectionAssembler assembler;
            for (auto& row : outputRows)
                assembler.add(row.type, row.sessionTime, std::move(row.json));
            outputRows = assembler.take();
        }
        for (auto& projected : outputRows) {
            const auto type = projected.type;
            auto& json = projected.json;
            if (type == 1) msg.telemetry.push_back(glz::raw_json{json});
            else if (type == 2) msg.statusHistory.push_back(glz::raw_json{json});
            else if (type == 3) msg.damageHistory.push_back(glz::raw_json{json});
            else if (type == 11) msg.motionHistory.push_back(glz::raw_json{json});
            else if (type == 12) msg.motionExHistory.push_back(glz::raw_json{json});
            else if (type == 4) {
                LapScanFields lap{};
                (void)glz::read<kPartialRead>(lap, json);
                msg.lapProgress.push_back({projected.sessionTime, lap.current_lap_ms,
                    lap.lap_distance_m, lap.sector, lap.s1_ms, lap.s2_ms});
            } else if (type == 13) {
                msg.positionsHistory.push_back(glz::raw_json{json});
                PositionsRow positions{};
                (void)glz::read<kPartialRead>(positions, json);
                const auto car = std::find_if(positions.cars.begin(), positions.cars.end(),
                    [&](const auto& value) { return value.idx == driverIndex; });
                if (car != positions.cars.end())
                    msg.playerPositions.push_back({projected.sessionTime, car->x, car->z});
            }
        }
        return writeJson(msg);
    }
    auto it = lapBlocks_.find(lapNum);
    if (it == lapBlocks_.end()) return {};
    const LapBlock& b = it->second;
    PlaybackLapDataRow msg;
    msg.lapNum           = b.lapNum;
    msg.startSessionTime = b.startSessionTime;
    msg.endSessionTime   = b.endSessionTime;
    msg.rowTypeMask      = rowTypeMask;
    if (!indexedArchive_) return {};
    auto* self = const_cast<TnrdV6Reader*>(this);
    const uint32_t savedOutputMask = self->playbackOutputRowMask_;
    self->playbackOutputRowMask_ = rowTypeMask;
    const uint32_t requestedMask = playbackDriverIndex_ >= 0
        ? expandedPlaybackMask(rowTypeMask) : rowTypeMask;
        // An indexed lap request is self-contained. Analysis can issue it in
        // the same render commit that publishes new page requirements, so
        // consulting the previous page's V6 history subscription can return a
        // partial lap while claiming the full row-family mask.
        const std::vector<uint8_t> selectedV6Types = v6TypesForRowMask(rowTypeMask);
    auto* v6 = indexedArchive_.get();
        if (v6) v6->setRequestedTypes(selectedV6Types);
        std::vector<detail::V4TimedRow> rows;std::string error;const uint32_t mask=requestedMask&(detail::v4TypeBit(1)|detail::v4TypeBit(2)|detail::v4TypeBit(3)|detail::v4TypeBit(4)|detail::v4TypeBit(11)|detail::v4TypeBit(12)|detail::v4TypeBit(13)|detail::v4TypeBit(7)|detail::v4TypeBit(8)|detail::v4TypeBit(9));
        if(!indexedArchive_.get()->rowsForLap((uint32_t)lapNum,mask,rows,&error)){if(v6)v6->setRequestedTypes(playbackV6Types_);self->playbackOutputRowMask_=savedOutputMask;return {};}
        if (v6) v6->setRequestedTypes(playbackV6Types_);
        std::vector<V6ProjectedRecord> outputRows;
        uint64_t outputOrder = 0;
        for(const auto&r:rows){
            if (
                !selectedV6Types.empty() &&
                std::find(selectedV6Types.begin(), selectedV6Types.end(), r.rowType) == selectedV6Types.end())
                continue;
            // A timed practice/quali lap number is reused across the in-lap,
            // garage and following out-lap. Those rows can share an indexed chunk
            // key with the next flying attempt, so honour the indexed flying
            // interval when materialising a lap.
            if(r.sessionTime<b.startSessionTime||r.sessionTime>b.endSessionTime)continue;
        std::vector<std::pair<uint8_t, std::string>> projected;
        projectV6Row(r.rowType, r.sessionTime, r.json, projected);
        for (auto& [type, json] : projected)
            outputRows.push_back({type, r.sessionTime, outputOrder++, std::move(json)});
    }
    if (!sparseV6Playback_) {
            V6ProjectionAssembler assembler;
            for (auto& row : outputRows)
                assembler.add(row.type, row.sessionTime, std::move(row.json));
            outputRows = assembler.take();
        }
        for (auto& projected : outputRows) {
            const auto type = projected.type;
            auto& json = projected.json;
            if(type==1)msg.telemetry.push_back(glz::raw_json{json});
            else if(type==2)msg.statusHistory.push_back(glz::raw_json{json});
            else if(type==3)msg.damageHistory.push_back(glz::raw_json{json});
            else if(type==11)msg.motionHistory.push_back(glz::raw_json{json});
            else if(type==12)msg.motionExHistory.push_back(glz::raw_json{json});
            else if(type==7)msg.timingHistory.push_back(glz::raw_json{json});
            else if(type==8)msg.participantsHistory.push_back(glz::raw_json{json});
            else if(type==9)msg.allStatusHistory.push_back(glz::raw_json{json});
            else if(type==4){LapScanFields lap{};(void)glz::read<kPartialRead>(lap,json);msg.lapProgress.push_back({projected.sessionTime,lap.current_lap_ms,lap.lap_distance_m,lap.sector,lap.s1_ms,lap.s2_ms});}
            else if(type==13){msg.positionsHistory.push_back(glz::raw_json{json});PositionsRow pos{};(void)glz::read<kPartialRead>(pos,json);const int idx=playbackDriverIndex_>=0?playbackDriverIndex_:pos.player_idx;const auto car=std::find_if(pos.cars.begin(),pos.cars.end(),[&](const auto& value){return value.idx==idx;});if(car!=pos.cars.end())msg.playerPositions.push_back({projected.sessionTime,car->x,car->z});}
        }
        self->playbackOutputRowMask_ = savedOutputMask;
        return writeJson(msg);
}

bool TnrdV6Reader::getAnalysisLapProgress(int lapNum, AnalysisLapProgress& out,
                                          int driverIndex) const {
    if (driverIndex >= 0 && driverIndex != playbackDriverIndex_) {
        auto* v6 = indexedArchive_.get();
        if (!v6 || !v6->driverHeader(static_cast<uint8_t>(driverIndex))) return false;
        std::optional<detail::V6LapSummary> selectedLap;
        for (const auto& lap : v6->driverLapSummaries(static_cast<uint8_t>(driverIndex))) {
            if (lap.phase == detail::V6Phase::Race &&
                static_cast<int>(lap.lapNumber) == lapNum) selectedLap = lap;
        }
        if (!selectedLap) return false;

        AnalysisLapProgress result;
        result.lapNum = lapNum;
        result.startSessionTime = selectedLap->startSessionTime;
        result.endSessionTime = selectedLap->endSessionTime;
        result.lapTimeMs = static_cast<int>(selectedLap->lapTimeMs);
        result.trackLengthM = static_cast<float>(trackLengthM_);
        std::vector<detail::V4TimedRow> rows;
        std::string error;
        if (!v6->readDriverLapTypes(static_cast<uint8_t>(driverIndex), selectedLap->lapId,
                                    {24}, rows, &error)) return false;
        for (const auto& row : rows) {
            if (row.sessionTime < result.startSessionTime || row.sessionTime > result.endSessionTime) continue;
            LapScanFields lap{};
            if (glz::read<kPartialRead>(lap, row.json)) continue;
            result.points.push_back({row.sessionTime, lap.current_lap_ms,
                lap.lap_distance_m, lap.sector, lap.s1_ms, lap.s2_ms});
        }
        if (result.points.empty()) return false;
        const auto [sector1, sector2] = sectorEndDistances(result.points);
        result.sector1EndDistanceM = sector1;
        result.sector2EndDistanceM = sector2;
        for (const auto& point : result.points) {
            if (point.s1_ms > 0) result.sector1TimeMs = point.s1_ms;
            if (point.s2_ms > 0) result.sector2TimeMs = point.s2_ms;
        }
        out = std::move(result);
        return true;
    }
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
            if (row.rowType != 24 || row.sessionTime < block.startSessionTime ||
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

void TnrdV6Reader::preparePlaybackLap() {
    const float inf=std::numeric_limits<float>::infinity();
    for(auto& lane:playbackLanes_){lane.chunks.clear();lane.nextChunk=0;lane.nextPrefetched=false;lane.rows.clear();lane.rowPos=0;lane.maxDecodedTime=-inf;lane.safeThrough=inf;}
    if(!indexedArchive_||!indexedArchive_->isOpen()||playbackLap_<0){playbackPrepared_=true;return;}
    const PlaybackChunkLookup lookup(*indexedArchive_);std::vector<size_t> selected;
    indexedArchive_->playbackChunkIndices(playbackRowMask_, selected);
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

bool TnrdV6Reader::loadPlaybackFrontier(float throughTime) {
    struct PendingChunk{size_t lane;size_t index;uint64_t sequence;};
    std::vector<PendingChunk> pending;const PlaybackChunkLookup lookup(*indexedArchive_);float priority=std::numeric_limits<float>::infinity();
    for(const auto& lane:playbackLanes_)if(lane.nextChunk<lane.chunks.size()&&lane.safeThrough<=throughTime)priority=std::min(priority,lane.safeThrough);
    for(size_t i=0;i<playbackLanes_.size();++i){const auto& lane=playbackLanes_[i];if(lane.nextChunk<lane.chunks.size()&&lane.safeThrough==priority){const size_t index=lane.chunks[lane.nextChunk];pending.push_back({i,index,lookup.sequence(index)});}}
    if(pending.empty())return false;
    std::sort(pending.begin(),pending.end(),[](const auto&a,const auto&b){return a.sequence<b.sequence;});
    std::vector<size_t> indices;indices.reserve(pending.size());for(const auto& item:pending)indices.push_back(item.index);
    std::string error;
    // Rows stay unrendered until pullUntil() emits them. A seek lands mid-lap
    // in every driver's chunk of each all-car family (positions, timing), and
    // rendering all of those laps here, only to drop the part before the
    // cursor, dominated the seek. Nothing needs rows at or before the cursor.
    auto* v6=indexedArchive_.get();
    std::vector<std::shared_ptr<const detail::V6RowSource>> v6Sources;std::vector<std::vector<detail::V6DeferredRow>> v6Rows;std::vector<float> v6MaxTime;
    if(v6){
        v6Sources.resize(indices.size());v6Rows.resize(indices.size());v6MaxTime.resize(indices.size());
        for(size_t i=0;i<indices.size();++i)if(!v6->deferredRowsForChunk(indices[i],playbackCursor_,v6Sources[i],v6Rows[i],v6MaxTime[i],&error)){lastError_=error;return false;}
    }
    // The app recorder accepts at most 200 ms of packet reordering before it
    // rewinds the indexed timeline. Hold that tail until the successor is known.
    const float inf=std::numeric_limits<float>::infinity();constexpr float REORDER_WINDOW_S=0.2f;
    auto before=[](const PlaybackRow&a,const PlaybackRow&b){if(a.t!=b.t)return a.t<b.t;return a.sequence<b.sequence;};
    for(size_t i=0;i<pending.size();++i){
        auto& lane=playbackLanes_[pending[i].lane];
        lane.nextPrefetched=false;
        if(lane.rowPos){lane.rows.erase(lane.rows.begin(),lane.rows.begin()+(ptrdiff_t)lane.rowPos);lane.rowPos=0;}
        const size_t retained=lane.rows.size();
            lane.maxDecodedTime=std::max(lane.maxDecodedTime,v6MaxTime[i]);
            for(const auto& row:v6Rows[i])lane.rows.push_back({row.sessionTime,pending[i].sequence,{},v6Sources[i],row.row});
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

void TnrdV6Reader::prefetchPlaybackChunk() {
    if(!indexedArchive_)return;const PlaybackChunkLookup lookup(*indexedArchive_);const size_t limit=4u;size_t outstanding=0;for(const auto& lane:playbackLanes_)if(lane.nextPrefetched)++outstanding;if(outstanding>=limit){playbackPrefetchOutstanding_=true;return;}
    std::vector<size_t> candidates;for(size_t i=0;i<playbackLanes_.size();++i){const auto& lane=playbackLanes_[i];if(!lane.nextPrefetched&&lane.nextChunk<lane.chunks.size())candidates.push_back(i);}std::stable_sort(candidates.begin(),candidates.end(),[&](size_t a,size_t b){const auto& left=playbackLanes_[a];const auto& right=playbackLanes_[b];if(left.safeThrough!=right.safeThrough)return left.safeThrough<right.safeThrough;return lookup.sequence(left.chunks[left.nextChunk])<lookup.sequence(right.chunks[right.nextChunk]);});
    for(size_t laneIndex:candidates){if(outstanding>=limit)break;auto& lane=playbackLanes_[laneIndex];lane.nextPrefetched=true;++outstanding;indexedArchive_->prefetchChunk(lane.chunks[lane.nextChunk]);}playbackPrefetchOutstanding_=outstanding!=0;
}

bool TnrdV6Reader::encodeHotRow(uint8_t type,std::string_view json,std::vector<uint8_t>& out){
    if(type==1){TelemetryRow row{};if(glz::read<kPartialRead>(row,json))return false;bin::encodeTelemetry(out,row);return true;}
    if(type==11){MotionRow row{};if(glz::read<kPartialRead>(row,json))return false;bin::encodeMotion(out,row);return true;}
    if(type==12){MotionExRow row{};if(glz::read<kPartialRead>(row,json))return false;bin::encodeMotionEx(out,row);return true;}
    return false;
}

std::vector<std::string> TnrdV6Reader::pullUntil(float t) {
    std::vector<std::string> out;
    if(!indexedArchive_)return out;
        if(!playbackPrepared_)preparePlaybackLap();
        const float inf=std::numeric_limits<float>::infinity();
        auto before=[](const PlaybackRow&a,const PlaybackRow&b){if(a.t!=b.t)return a.t<b.t;return a.sequence<b.sequence;};
        for(;;){
            size_t best=playbackLanes_.size();float safeThrough=inf;bool futureChunk=false;
            for(size_t i=0;i<playbackLanes_.size();++i){const auto& lane=playbackLanes_[i];safeThrough=std::min(safeThrough,lane.safeThrough);futureChunk|=lane.nextChunk<lane.chunks.size();if(lane.rowPos<lane.rows.size()&&(best==playbackLanes_.size()||before(lane.rows[lane.rowPos],playbackLanes_[best].rows[playbackLanes_[best].rowPos])))best=i;}
            if(best!=playbackLanes_.size()&&playbackLanes_[best].rows[playbackLanes_[best].rowPos].t<=t&&(playbackLanes_[best].rows[playbackLanes_[best].rowPos].t<safeThrough||!futureChunk)){auto& lane=playbackLanes_[best];auto& row=lane.rows[lane.rowPos++];if(row.source){row.json=row.source->render(row.sourceRow);row.source.reset();setSessionTime(row.json,row.t);tagV6StoredType(row.json,static_cast<uint8_t>(best));}out.push_back(std::move(row.json));continue;}
            const float through=best!=playbackLanes_.size()&&playbackLanes_[best].rows[playbackLanes_[best].rowPos].t<=t?playbackLanes_[best].rows[playbackLanes_[best].rowPos].t:t;
            if(futureChunk&&safeThrough<=through){if(loadPlaybackFrontier(through))continue;break;}
        // The lap directory follows the recorded player, so lanes cover the
        // whole timeline and never advance lap by lap.
            break;
        }
        prefetchPlaybackChunk();return out;
}

void TnrdV6Reader::pullUntilSplit(float t, std::string& jsonOut, std::vector<uint8_t>& binOut,
                                  uint32_t& seenTypes,
                                  std::array<std::string, 16>* lastOfType) {
    const float cadenceEnd = std::isfinite(t) ? t : totalTime_;
    if(!indexedArchive_)return;
        if ( playbackDriverIndex_ >= 0) {
            auto rows = pullUntil(t);
            std::vector<V6ProjectedRecord> projectedRows;
            uint64_t projectedOrder = 0;
            for (auto& source : rows) {
                const uint8_t sourceType = scanV6StoredType(source);
                const float sourceTime = scanSessionTime(source.data(), static_cast<int>(source.size()));
                std::vector<std::pair<uint8_t, std::string>> projected;
                projectV6Row(sourceType, sourceTime, source, projected);
                for (auto& [type, row] : projected)
                    projectedRows.push_back({
                        type, sourceTime, projectedOrder++, std::move(row),
                    });
            }
            // The in-engine strategy reducer still consumes complete legacy
            // dependency rows. Keep that one opt-in page on the compatibility
            // projection until the reducer itself accepts V6 field patches.
            const bool sparseOutput = sparseV6Playback_ &&
                !(playbackOutputRowMask_ & detail::v4TypeBit(15));
            if (!sparseOutput) {
                V6ProjectionAssembler assembler;
                for (const uint8_t type : {uint8_t{7}, uint8_t{9}, uint8_t{13}})
                    assembler.seed(type, v6ProjectionState_[type]);
                for (auto& row : projectedRows)
                    assembler.add(row.type, row.sessionTime, std::move(row.json));
                projectedRows = assembler.take();
                for (const auto& row : projectedRows)
                    if (row.type == 7 || row.type == 9 || row.type == 13)
                        v6ProjectionState_[row.type] = row.json;
            }
            if (auto* v6 = indexedArchive_.get()) {
                const auto& shared = v6->sharedRecords();
                const float through = std::isfinite(t) ? t : totalTime_;
                while (v6SharedPos_ < v6SharedOrder_.size()) {
                    const auto& record = shared[v6SharedOrder_[v6SharedPos_]];
                    const float time = v6->logicalTime(record.phase, record.sessionTime);
                    if (time > through) break;
                    ++v6SharedPos_;
                    const uint8_t type = scanType(record.json.data(), static_cast<int>(record.json.size()));
                    if (!(playbackRowMask_ & detail::v4TypeBit(type))) continue;
                    std::string json = record.json;
                    setSessionTime(json, time);
                    projectedRows.push_back({type, time, projectedOrder++, std::move(json)});
                }
                std::stable_sort(projectedRows.begin(), projectedRows.end(),
                    [](const auto& left, const auto& right) {
                        return std::tie(left.sessionTime, left.order) <
                               std::tie(right.sessionTime, right.order);
                    });
            }
            for (auto& projected : projectedRows) {
                const uint8_t type = projected.type;
                auto& row = projected.json;
                seenTypes |= detail::v4TypeBit(type);
                if (type == 3) {
                    // V6 stores the actual 10 Hz UDP packet stream, so emit
                    // each persisted sample directly. Cadence synthesis is
                    // retained only for deduplicated V1-V5 recordings.
                    jsonOut += row;
                    jsonOut.push_back('\n');
                    if (lastOfType) (*lastOfType)[3] = row;
                    continue;
                }
                jsonOut += row;
                jsonOut.push_back('\n');
                if (lastOfType && type < lastOfType->size()) (*lastOfType)[type] = row;
            }
            damageCadenceCursor_ = cadenceEnd;
            return;
        }
    // No playback driver: forward the rows as stored. V6 persists every damage
    // packet, so nothing is reconstructed at a fixed cadence.
    for (auto& row : pullUntil(t)) {
        const uint8_t tid = scanType(row.data(), (int)row.size());
        if (!(playbackRowMask_ & (1u << tid))) continue;
        seenTypes |= (1u << tid);
        if (tid == 1 || tid == 11 || tid == 12) { (void)encodeHotRow(tid, row, binOut); continue; }
        jsonOut += row;
        jsonOut.push_back('\n');
        if (lastOfType && tid < lastOfType->size()) (*lastOfType)[tid] = row;
    }
    damageCadenceCursor_ = cadenceEnd;
}

TnrdV6Reader::SeekFlush TnrdV6Reader::seekFlush(float target, float currentLapStart,
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
    auto* v6 = indexedArchive_.get();
            const auto& historyTypes = historyV6Types();
            if (v6) v6->setRequestedTypes(historyTypes);
            const uint32_t savedOutputMask = playbackOutputRowMask_;
            playbackOutputRowMask_ = mask;
            const uint32_t sourceMask = expandedPlaybackMask(mask);
            std::string error;
            V6ProjectionAssembler compatibilityAssembler;
            const auto appendProjected = [&](const V4TimedRow& source,
                                             float outputTime) {
                std::vector<std::pair<uint8_t, std::string>> projected;
                projectV6Row(source.rowType, outputTime, source.json, projected);
                for (auto& [type, row] : projected) {
                    if (sparseV6Playback_) {
                        f.coldJson += row;
                        f.coldJson.push_back('\n');
                    } else {
                        compatibilityAssembler.add(type, outputTime, std::move(row));
                    }
                }
            };

            // V6 stores independent fields, so a range beginning at a lap or
            // finite-window boundary also needs the last value immediately
            // before that boundary. Without this seed, ERS/fuel/wear charts
            // retain a default or stale value until their first later update.
            if (v6 && !historyTypes.empty()) {
                std::vector<uint8_t> seedTypes;
                seedTypes.reserve(historyTypes.size());
                for (const uint8_t type : historyTypes)
                    if (type > 0 && type <= 22) seedTypes.push_back(type);
                if (!seedTypes.empty()) {
                    std::vector<V4TimedRow> seeds;
                    if (!v6->latestRows(windowStart, seedTypes, seeds, &error, cancelled)) {
                        playbackOutputRowMask_ = savedOutputMask;
                        v6->setRequestedTypes(playbackV6Types_);
                        if (!cancelled || !cancelled()) lastError_ = std::move(error);
                        return f;
                    }
                    for (const auto& seed : seeds) {
                        if (cancelled && cancelled()) {
                            playbackOutputRowMask_ = savedOutputMask;
                            v6->setRequestedTypes(playbackV6Types_);
                            return f;
                        }
                        appendProjected(seed, windowStart);
                    }
                }
            }
            // V6 rows are already split by consumer data type. Forward each
            // sparse patch independently so a narrow history request never
            // rebuilds and serializes a complete legacy row first.
            const bool loaded = indexedArchive_->forEachRowInRange(
                windowStart, target, sourceMask,
                [&](const V4TimedRow& source) {
                    if (cancelled && cancelled()) return false;
                    appendProjected(source, source.sessionTime);
                    return true;
                }, &error, cancelled);
            if (!loaded) {
                playbackOutputRowMask_ = savedOutputMask;
                if (v6) v6->setRequestedTypes(playbackV6Types_);
                if (!cancelled || !cancelled()) lastError_ = std::move(error);
                return f;
            }
            if (!sparseV6Playback_) {
                auto binary = std::make_shared<std::vector<uint8_t>>();
                for (auto& projected : compatibilityAssembler.take()) {
                    if (projected.type == 1 || projected.type == 11 || projected.type == 12) {
                        if (!encodeHotRow(projected.type, projected.json, *binary)) {
                            playbackOutputRowMask_ = savedOutputMask;
                            if (v6) v6->setRequestedTypes(playbackV6Types_);
                            lastError_ = "could not encode assembled V6 seek row";
                            return f;
                        }
                    } else {
                        f.coldJson += projected.json;
                        f.coldJson.push_back('\n');
                    }
                }
                if (!binary->empty()) {
                    f.binaryStore = std::move(binary);
                    f.binaryBegin = 0;
                    f.binaryEnd = f.binaryStore->size();
                }
            }
            playbackOutputRowMask_ = savedOutputMask;
            if (v6) v6->setRequestedTypes(playbackV6Types_);
            if (!f.coldJson.empty()) f.coldJson.pop_back();
            return f;
}

std::unique_ptr<TnrdReader::HistoryReader> TnrdV6Reader::openHistoryReader(std::string* errorOut) const {
    if (v6Path_.empty()) {
        if (errorOut) *errorOut = "this V6 recording was not opened from a file";
        return {};
    }
    auto archive = std::make_unique<TnrdV6Archive>();
    HeaderRow ignored;
    if (!archive->open(v6Path_, ignored, errorOut)) return {};
    return std::make_unique<V6HistoryReader>(std::move(archive));
}

bool TnrdV6Reader::historyRequest(float from, float to, uint32_t mask,
                                  TnrdReader::HistoryRequest& out) const {
    // Column blocks are what the binary, sparse playback contract consumes.
    if (!binaryPlayback_ || !sparseV6Playback_) return false;
    const int driver = effectivePlaybackDriver();
    if (driver < 0 || driver >= 24) return false;
    out.driver = static_cast<uint8_t>(driver);
    // The same fields and seeding as seekFlush().
    out.types = historyV6Types();
    out.seed = !out.types.empty();
    out.mask = mask;
    out.from = from;
    out.to = to;
    return true;
}

std::vector<TrackStatusChange> TnrdV6Reader::aeroTrackStatusTimeline() const {
    std::vector<TrackStatusChange> timeline;
    if (auto* v6 = indexedArchive_.get()) {
        // Shared records are stored in file order; analysis laps are race laps.
        std::vector<const V6SharedRecord*> race;
        for (const auto& record : v6->sharedRecords())
            if (record.phase == V6Phase::Race) race.push_back(&record);
        std::stable_sort(race.begin(), race.end(), [](const auto* a, const auto* b) {
            return a->sessionTime < b->sessionTime;
        });
        for (const auto* record : race) appendTrackStatus(timeline, record->sessionTime, record->json);
        return timeline;
    }
    return timeline;
}

std::vector<int> TnrdV6Reader::analysisLapNumbers(int driverIndex) const {
    std::vector<int> laps;
    if (driverIndex >= 0 && driverIndex != playbackDriverIndex_) {
        auto* v6 = indexedArchive_.get();
        if (!v6 || !v6->driverHeader(static_cast<uint8_t>(driverIndex))) return laps;
        for (const auto& lap : v6->driverLapSummaries(static_cast<uint8_t>(driverIndex))) {
            const int lapNum = static_cast<int>(lap.lapNumber);
            if (lap.phase == detail::V6Phase::Race && lapNum > 0 &&
                std::find(laps.begin(), laps.end(), lapNum) == laps.end()) laps.push_back(lapNum);
        }
        return laps;
    }
    for (const auto& [lapNum, block] : lapBlocks_) laps.push_back(lapNum);
    return laps;
}

bool TnrdV6Reader::getAnalysisErsSamples(int lapNum, std::vector<ErsSample>& out,
                                         int driverIndex) const {
    // The two columns straight from the archive, no JSON either way.
    if (auto* v6 = indexedArchive_.get()) {
        AnalysisLapProgress lap;
        if (!getAnalysisLapProgress(lapNum, lap, driverIndex)) return false;
        const int car = analysisCarIndex(driverIndex);
        if (car < 0 || car >= 24) return false;
        std::vector<std::pair<float, double>> power;
        std::vector<std::pair<float, double>> store;
        std::string error;
        if (!v6->fieldSamples(static_cast<uint8_t>(car), detail::V6DataType::EnginePower,
                              "engine_power_mguk_kw", lap.startSessionTime, lap.endSessionTime,
                              power, &error) ||
            !v6->fieldSamples(static_cast<uint8_t>(car), detail::V6DataType::ERSStore,
                              "ers_j", lap.startSessionTime, lap.endSessionTime, store, &error))
            return false;
        // Each power sample takes the store's latest value at or before it.
        out.clear();
        out.reserve(power.size());
        size_t s = 0;
        double ersJ = store.empty() ? 0.0 : store.front().second;
        for (const auto& [time, kw] : power) {
            while (s < store.size() && store[s].first <= time) ersJ = store[s++].second;
            if (std::isfinite(kw)) out.push_back({time, static_cast<float>(kw), ersJ});
        }
        return true;
    }
    return false;
}

bool TnrdV6Reader::getAnalysisHarvestSamples(int lapNum, std::vector<HarvestSample>& out,
                                             int driverIndex) const {
    if (auto* v6 = indexedArchive_.get()) {
        AnalysisLapProgress lap;
        if (!getAnalysisLapProgress(lapNum, lap, driverIndex)) return false;
        const int car = analysisCarIndex(driverIndex);
        if (car < 0 || car >= 24) return false;
        return v6HarvestSamples(*v6, static_cast<uint8_t>(car), lap.startSessionTime,
                                lap.endSessionTime, out);
    }
    return false;
}

bool TnrdV6Reader::getAnalysisInputSamples(int lapNum, std::vector<InputSample>& out,
                                           int driverIndex) const {
    // The pedal and drive columns straight from the archive.
    if (auto* v6 = indexedArchive_.get()) {
        AnalysisLapProgress lap;
        if (!getAnalysisLapProgress(lapNum, lap, driverIndex)) return false;
        const int car = analysisCarIndex(driverIndex);
        if (car < 0 || car >= 24) return false;
        constexpr size_t kColumns = 6;
        std::array<std::vector<std::pair<float, double>>, kColumns> columns;
        const std::array<std::pair<detail::V6DataType, const char*>, kColumns> fields{{
            {detail::V6DataType::Throttle, "throttle"}, {detail::V6DataType::Brake, "brake"},
            {detail::V6DataType::Speed, "speed_kph"}, {detail::V6DataType::RPM, "rpm"},
            {detail::V6DataType::Gear, "gear"}, {detail::V6DataType::Steering, "steering"}}};
        std::string error;
        const uint8_t id = static_cast<uint8_t>(car);
        for (size_t c = 0; c < kColumns; ++c)
            if (!v6->fieldSamples(id, fields[c].first, fields[c].second,
                                  lap.startSessionTime, lap.endSessionTime, columns[c], &error))
                return false;
        // One sample per moment any column changed, each carrying the latest
        // value of every column.
        out.clear();
        out.reserve(columns[2].size());
        std::array<size_t, kColumns> at{};
        std::array<double, kColumns> now{};
        for (;;) {
            float time = std::numeric_limits<float>::infinity();
            for (size_t c = 0; c < kColumns; ++c)
                if (at[c] < columns[c].size()) time = std::min(time, columns[c][at[c]].first);
            if (!std::isfinite(time)) break;
            for (size_t c = 0; c < kColumns; ++c)
                while (at[c] < columns[c].size() && columns[c][at[c]].first == time)
                    now[c] = columns[c][at[c]++].second;
            out.push_back({time, static_cast<float>(now[0]), static_cast<float>(now[1]),
                           static_cast<float>(now[2]), static_cast<float>(now[3]),
                           static_cast<int>(std::lround(now[4])), static_cast<float>(now[5])});
        }
        return true;
    }
    return false;
}

bool TnrdV6Reader::getAnalysisPositionSamples(int lapNum, std::vector<PlayerPositionPoint>& out,
                                              int driverIndex) const {
    // The two position columns straight from the archive.
    if (auto* v6 = indexedArchive_.get()) {
        AnalysisLapProgress lap;
        if (!getAnalysisLapProgress(lapNum, lap, driverIndex)) return false;
        const int car = analysisCarIndex(driverIndex);
        if (car < 0 || car >= 24) return false;
        std::vector<std::pair<float, double>> xs, zs;
        std::string error;
        const uint8_t id = static_cast<uint8_t>(car);
        if (!v6->fieldSamples(id, detail::V6DataType::Position, "x", lap.startSessionTime,
                              lap.endSessionTime, xs, &error) ||
            !v6->fieldSamples(id, detail::V6DataType::Position, "z", lap.startSessionTime,
                              lap.endSessionTime, zs, &error)) return false;
        out.clear();
        size_t j = 0;
        for (const auto& [time, x] : xs) {
            while (j < zs.size() && zs[j].first < time) ++j;
            if (j < zs.size() && zs[j].first == time) out.push_back({time, x, zs[j++].second});
        }
        return true;
    }
    return false;
}

std::shared_ptr<ErsPeakSweep> TnrdV6Reader::startErsPeakSweep() {
    if (v6Path_.empty()) return {};
    auto* v6 = indexedArchive_.get();
    if (!v6) return {};

    struct SweepLap { float start; float end; };
    struct SweepCar { int driver; std::vector<SweepLap> laps; };
    auto cars = std::make_shared<std::vector<SweepCar>>();
    for (int driver = 0; driver < 24; ++driver) {
        if (!v6->driverHeader(static_cast<uint8_t>(driver))) continue;
        SweepCar car{driver, {}};
        for (const auto& lap : v6->driverLapSummaries(static_cast<uint8_t>(driver)))
            if (lap.phase == detail::V6Phase::Race)
                car.laps.push_back({lap.startSessionTime, lap.endSessionTime});
        if (!car.laps.empty()) cars->push_back(std::move(car));
    }
    if (cars->empty()) return {};

    std::vector<int> drivers;
    for (const auto& car : *cars) drivers.push_back(car.driver);
    auto sweep = std::make_shared<ErsPeakSweep>(drivers);

    // Each worker opens its own archive (one instance takes one call at a
    // time) and takes cars from a shared queue until none are left.
    const auto timeline = std::make_shared<const std::vector<TrackStatusChange>>(aeroTrackStatusTimeline());
    auto next = std::make_shared<std::atomic<size_t>>(0);
    const unsigned hardware = std::max(1u, std::thread::hardware_concurrency());
    const size_t workers = std::min<size_t>(cars->size(), std::min(4u, hardware));
    size_t started = 0;
    for (size_t w = 0; w < workers; ++w) {
        try {
            std::thread([sweep, cars, next, timeline, path = v6Path_] {
                detail::V6LoadResult loaded;
                std::string error;
                bool open = false;
                try {
                    open = detail::TNRD_V6::load(path, loaded, error) && loaded.archive;
                } catch (...) {}
                std::vector<std::pair<float, double>> power;
                std::vector<ErsSample> samples;
                std::vector<HarvestSample> harvest;
                for (size_t index = (*next)++; index < cars->size(); index = (*next)++) {
                    const auto& car = (*cars)[index];
                    if (!open || sweep->cancelled()) {
                        sweep->publish(car.driver, std::nullopt);
                        continue;
                    }
                    // Every car taken is published, or a map waiting on it would
                    // never return.
                    std::optional<ErsPeaks> result;
                    try {
                        ErsPeaks peaks;
                        // The engine-power column for each lap, read raw.
                        for (const auto& lap : car.laps) {
                            if (sweep->cancelled()) break;
                            if (!loaded.archive->fieldSamples(static_cast<uint8_t>(car.driver),
                                    detail::V6DataType::EnginePower, "engine_power_mguk_kw",
                                    lap.start, lap.end, power, &error,
                                    [&sweep] { return sweep->cancelled(); })) continue;
                            samples.clear();
                            samples.reserve(power.size());
                            for (const auto& [time, kw] : power)
                                if (std::isfinite(kw)) samples.push_back({time, static_cast<float>(kw), 0.0});
                            accumulateErsPeaks(peaks, samples, lap.start, lap.end, *timeline);
                            if (v6HarvestSamples(*loaded.archive, static_cast<uint8_t>(car.driver),
                                    lap.start, lap.end, harvest, [&sweep] { return sweep->cancelled(); }))
                                accumulateHarvestPeak(peaks, harvest, lap.start, lap.end);
                        }
                        if (!sweep->cancelled()) result = peaks;
                    } catch (...) {}
                    sweep->publish(car.driver, result);
                }
            }).detach();
            ++started;
        } catch (const std::system_error&) {
            break;
        }
    }
    // With no worker running nobody would publish; waiters fall back to their
    // own scan instead.
    if (started == 0) sweep->cancel();
    return sweep;
}

std::shared_ptr<TrackCornerSweep> TnrdV6Reader::startTrackCornerSweep() {
    if (v6Path_.empty() || trackLengthM_ <= 0) return {};
    auto* v6 = indexedArchive_.get();
    if (!v6) return {};

    struct SweepLap { float start; float end; };
    struct SweepCar { int driver; std::vector<SweepLap> laps; };
    auto cars = std::make_shared<std::vector<SweepCar>>();
    for (int driver = 0; driver < 24; ++driver) {
        if (!v6->driverHeader(static_cast<uint8_t>(driver))) continue;
        SweepCar car{driver, {}};
        for (const auto& lap : v6->driverLapSummaries(static_cast<uint8_t>(driver)))
            if (lap.phase == detail::V6Phase::Race && lap.isCompleted && !lap.isPartial &&
                lap.lapNumber > 0 && lap.endSessionTime > lap.startSessionTime)
                car.laps.push_back({lap.startSessionTime, lap.endSessionTime});
        if (!car.laps.empty()) cars->push_back(std::move(car));
    }
    if (cars->empty()) return {};

    auto sweep = std::make_shared<TrackCornerSweep>();

    // Each worker opens its own archive (one instance takes one call at a
    // time), takes cars from a shared queue and adds their laps to its own
    // average; the last one to finish merges them all and publishes.
    struct Shared {
        std::mutex mutex;
        TrackPathAverage path;
        size_t running{};
        explicit Shared(float length) : path(length) {}
    };
    const float length = static_cast<float>(trackLengthM_);
    auto shared = std::make_shared<Shared>(length);
    auto next = std::make_shared<std::atomic<size_t>>(0);
    const unsigned hardware = std::max(1u, std::thread::hardware_concurrency());
    const size_t workers = std::min<size_t>(cars->size(), std::min(4u, hardware));
    shared->running = workers;
    const auto finish = [sweep, shared](const TrackPathAverage* path) {
        std::lock_guard<std::mutex> lock(shared->mutex);
        if (path) shared->path.merge(*path);
        if (--shared->running > 0 || sweep->cancelled()) return;
        TrackCorners corners = shared->path.corners();
        sweep->publish(corners.corners.empty() ? std::nullopt : std::optional<TrackCorners>(std::move(corners)));
    };
    size_t started = 0;
    for (size_t w = 0; w < workers; ++w) {
        try {
            std::thread([sweep, cars, next, finish, length, path = v6Path_] {
                TrackPathAverage average(length);
                try {
                    detail::V6LoadResult loaded;
                    std::string error;
                    const bool open = detail::TNRD_V6::load(path, loaded, error) && loaded.archive;
                    const auto stop = [&sweep] { return sweep->cancelled(); };
                    const auto within = [](const std::vector<std::pair<float, double>>& values, float from, float to) {
                        const auto less = [](const std::pair<float, double>& value, float time) {
                            return value.first < time;
                        };
                        return std::pair{std::lower_bound(values.begin(), values.end(), from, less),
                                         std::upper_bound(values.begin(), values.end(), to,
                                             [](float time, const std::pair<float, double>& value) {
                                                 return time < value.first;
                                             })};
                    };
                    std::vector<std::pair<float, double>> xs, zs, distance, pit, lapDistance;
                    std::vector<float> t;
                    std::vector<double> x, z;
                    for (size_t index = (*next)++; open && index < cars->size(); index = (*next)++) {
                        if (sweep->cancelled()) break;
                        const auto& car = (*cars)[index];
                        const auto id = static_cast<uint8_t>(car.driver);
                        auto& archive = *loaded.archive;
                        // The car's whole race at once, then cut into laps.
                        const float from = car.laps.front().start, to = car.laps.back().end;
                        if (!archive.fieldSamples(id, detail::V6DataType::Position, "x", from, to, xs, &error, stop) ||
                            !archive.fieldSamples(id, detail::V6DataType::Position, "z", from, to, zs, &error, stop) ||
                            !archive.fieldSamples(id, detail::V6DataType::LapTiming, "lap_distance_m",
                                                  from, to, distance, &error, stop) ||
                            !archive.fieldSamples(id, detail::V6DataType::LapTiming, "pit_status",
                                                  from, to, pit, &error, stop)) continue;
                        for (const auto& lap : car.laps) {
                            // Pit laps take the pit lane.
                            const auto [p0, p1] = within(pit, lap.start, lap.end);
                            if (std::any_of(p0, p1, [](const auto& value) { return value.second != 0.0; })) continue;
                            t.clear(); x.clear(); z.clear();
                            const auto [x0, x1] = within(xs, lap.start, lap.end);
                            auto zi = within(zs, lap.start, lap.end).first;
                            for (auto xi = x0; xi != x1; ++xi) {
                                while (zi != zs.end() && zi->first < xi->first) ++zi;
                                if (zi == zs.end()) break;
                                if (zi->first != xi->first) continue;
                                t.push_back(xi->first);
                                x.push_back(xi->second);
                                z.push_back(zi->second);
                            }
                            const auto [d0, d1] = within(distance, lap.start, lap.end);
                            lapDistance.assign(d0, d1);
                            (void)average.addLap(t, x, z, lapDistance);
                        }
                    }
                } catch (...) {}
                finish(&average);
            }).detach();
            ++started;
        } catch (const std::system_error&) {
            break;
        }
    }
    // Workers that never started count as finished; with none at all,
    // waiters fall back to each lap's own corners.
    for (size_t w = started; w < workers; ++w) finish(nullptr);
    if (started == 0) sweep->cancel();
    return sweep;
}

StrategySnapshotRow TnrdV6Reader::strategySnapshotAt(float t, StrategyProcessor* restoredProcessor,
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

std::string TnrdV6Reader::lapBlocksMessage() const {
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
    msg.tnrdVersion = "TNRD_V6";
    msg.deltaAvailable = true;
    msg.lapDistanceAvailable = true;
    msg.trackLengthM = trackLengthM_;
    addAnalysisDrivers(msg);
    msg.events.reserve(scannedEvents_.size());
    for (const auto& s : scannedEvents_) msg.events.push_back(glz::raw_json{ s });
    for (const auto& l : scannedLaps_)   msg.laps.push_back({ l.lapNum, l.lapTimeMs });
    return writeJson(msg);
}


bool TnrdV6Reader::exportParquet(const ParquetExportRequest& request, std::string* errorOut) {
    if (!indexedArchive_) {
        if (errorOut) *errorOut = "no .tnrd loaded";
        return false;
    }
    return exportTnrdV6Parquet(*indexedArchive_, request, errorOut);
}

} // namespace

std::unique_ptr<TnrdReaderBackend> makeTnrdV6Reader(TnrdReaderContext& context) {
    return std::make_unique<TnrdV6Reader>(context);
}

std::unique_ptr<TnrdReaderBackend> makeTnrdV6StrategyReader(
    TnrdReaderContext& context, std::unique_ptr<TnrdV6Archive> archive, const HeaderRow& header) {
    auto reader = std::make_unique<TnrdV6Reader>(context);
    reader->adoptForStrategy(std::move(archive), header);
    return reader;
}

bool summarizeTnrdV6(const std::string& path, TnrdReader::Summary& out, std::string& error) {
    V6LoadResult loaded;
    if (!TNRD_V6::load(path, loaded, error)) return false;
    out.header = std::move(loaded.header);
    out.drivers = static_cast<int>(loaded.archive->driverHeaders().size());
    out.durationSeconds = std::max(0.0f, loaded.archive->totalTime() - loaded.archive->startTime());
    return true;
}

} // namespace tnrp::detail
