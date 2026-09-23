#include "tnrp/TnrdReader.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <limits>
#include <iterator>
#include <optional>
#include <string_view>
#include <thread>
#include <unordered_set>

#ifndef _WIN32
#include <sys/types.h>
#endif

#include "tnrp/BinaryRows.h"
#include "TnrdCodec.h"
#include "tnrd/TNRD_V1.h"
#include "tnrd/TNRD_V2.h"
#include "tnrd/TNRD_V3.h"
#include "tnrd/TNRD_V4.h"
#include "tnrd/TNRD_V5.h"
#include "tnrd/TNRD_V6.h"

namespace tnrp {

// Minimal struct for pulling just the lap fields out of a "lap" row with glaze.
// error_on_unknown_keys=false ignores every other key; missing keys keep defaults.
// Must have external linkage (not in an anonymous namespace): glaze's
// compile-time reflection takes the type's mangled name, which MSVC refuses
// to do for internal-linkage types (error C7631). GCC/Clang allow it.
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
namespace {
constexpr glz::opts kPartialRead{ .null_terminated = false, .error_on_unknown_keys = false };

std::vector<uint8_t> v6TypesForRowMask(uint32_t mask) {
    std::vector<uint8_t> out;
    const auto add = [&](std::initializer_list<uint8_t> values) {
        out.insert(out.end(), values.begin(), values.end());
    };
    if (mask & detail::v4TypeBit(1)) add({1,2,3,4,5,6,7,8,9,10,11});
    if (mask & detail::v4TypeBit(2)) add({7,13,15,16,17,18,19,20});
    if (mask & detail::v4TypeBit(9)) add({7,13,15,16,17,18,19,20});
    if (mask & detail::v4TypeBit(10)) add({13});
    if (mask & detail::v4TypeBit(3)) add({12,14});
    if (mask & (detail::v4TypeBit(4) | detail::v4TypeBit(7))) add({24});
    if (mask & detail::v4TypeBit(11)) add({21});
    if (mask & detail::v4TypeBit(12)) add({22});
    if (mask & detail::v4TypeBit(13)) add({23});
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
        for (uint8_t type = 0; type < pending_.size(); ++type)
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

    std::array<glz::generic, 16> states_{};
    std::array<bool, 16> ready_{};
    std::array<bool, 16> pending_{};
    std::array<float, 16> pendingTime_{};
    std::array<uint64_t, 16> pendingOrder_{};
    std::vector<V6ProjectedRecord> records_;
    uint64_t nextOrder_{};
    TimingRow timing_{};
    AllStatusRow allStatus_{};
    PositionsRow positions_{};
};

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
}

// State types reconstructed on seek (panel-state setters, emitted individually):
// session, lap, timing, participants, all_status, tyre_sets.
static constexpr uint8_t STATE_TYPE_IDS[] = { 4, 5, 7, 8, 9, 10 };

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

TnrdReader::TnrdReader() = default;
TnrdReader::~TnrdReader() { close(); }

bool TnrdReader::wasRecoveredV6() const {
    if (loadedFormat_ != TnrdFormat::ChunkedV6 || !indexedArchive_) return false;
    const auto* v6 = dynamic_cast<const detail::TnrdV6Archive*>(indexedArchive_.get());
    return v6 && v6->wasRecovered();
}
bool TnrdReader::isLoaded() const {
    return tempFile_ != nullptr || (indexedArchive_ && indexedArchive_->isOpen());
}

bool TnrdReader::hasMore() const {
    if (!isChunkedTnrd(loadedFormat_)) return playPos_ < index_.size();
    if (!indexedArchive_ || !indexedArchive_->isOpen()) return false;
    if(!v4PlaybackPrepared_)return true;
    for(const auto& lane:v4PlaybackLanes_)if(lane.rowPos<lane.rows.size()||lane.nextChunk<lane.chunks.size())return true;
    const auto& laps=indexedArchive_->laps();if(v4PlaybackLap_==0)return !laps.empty();const auto it=std::find_if(laps.begin(),laps.end(),[&](const auto&l){return (int)l.lapNumber==v4PlaybackLap_;});return it!=laps.end()&&std::next(it)!=laps.end();
}

void TnrdReader::sweepStaleTempFiles() noexcept {
    try {
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path dir = fs::temp_directory_path(ec);
        if (ec) return;

        // Compare path::string_type directly (wstring on Windows, string on
        // POSIX). Converting every arbitrary temp entry with path::string()
        // can throw ERROR_NO_UNICODE_TRANSLATION before we even inspect it.
        const auto prefix = fs::path("tracknrace_").native();
        const auto suffix = fs::path(".tmp").native();

        int removed = 0;
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            const fs::path& p = it->path();
            const auto name = p.filename().native();
            // Match both apps' decompression temps: "tracknrace_*.tmp" (covers
            // current "tracknrace_temp_*" and legacy "tracknrace_<ts>.tmp").
            if (!name.starts_with(prefix) || !name.ends_with(suffix)) continue;
            std::error_code rmEc;
            if (fs::remove(p, rmEc)) ++removed;   // a held-open file simply fails; skip it
        }
        if (removed > 0)
            std::fprintf(stderr, "[tnrd] startup sweep: removed %d stale temp file(s)\n", removed);
    } catch (const std::exception& err) {
        // Startup cleanup is opportunistic. Report an unexpected filesystem
        // failure, but never let it escape through N-API or terminate Qt.
        std::fprintf(stderr, "[tnrd] startup sweep skipped: %s\n", err.what());
    } catch (...) {
        std::fprintf(stderr, "[tnrd] startup sweep skipped: unknown error\n");
    }
}

bool TnrdReader::buildIndex(const std::string& filePath, const std::string* memoryFile) {
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

    std::FILE* f = memoryFile ? nullptr : detail::openTnrdFile(filePath, "rb");
    if (!memoryFile && !f) {
        std::fprintf(stderr, "[tnrd] buildIndex: cannot open '%s'\n", filePath.c_str());
        return false;
    }
    // Reserve against vector regrowth during the scan: rows average well under
    // ~96 bytes of JSONL, so this slightly over-reserves and settles in one go.
    {
        const std::int64_t sz = memoryFile ? (std::int64_t)memoryFile->size()
            : (seekFile(f, 0, SEEK_END) ? tellFile(f) : -1);
        if (sz < 0 || (!memoryFile && !seekFile(f, 0, SEEK_SET))) { if (f) std::fclose(f); return false; }
        if (sz > 0) index_.reserve((size_t)sz / 96);
    }
    size_t memoryPos = 0;
    if (memoryFile) {
        memoryPos = memoryFile->find('\n');
        if (memoryPos == std::string::npos) return false;
        ++memoryPos;
    } else {
        int c; while ((c = std::fgetc(f)) != EOF && c != '\n') {}
    }
    if (binaryPlayback_) hotCum_.push_back(0);

    constexpr size_t CHUNK = 2 * 1024 * 1024;
    std::vector<char> buf(CHUNK);
    std::string partial;
    std::int64_t partialOffset = memoryFile ? (std::int64_t)memoryPos : tellFile(f);
    if (partialOffset < 0) { if (f) std::fclose(f); return false; }
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
                if (!hasExactTnrdIndex(loadedFormat_)) {
                    telRow.rev_lights_pct.reset();
                    telRow.rev_lights_bit_value.reset();
                }
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
                    if ((loadedFormat_ == TnrdFormat::ZstdV3 || isChunkedTnrd(loadedFormat_)) &&
                        trackLengthM_ > 0 &&
                        lf.last_lap_ms > 0 &&
                        (it->second.lapProgress.empty() ||
                         it->second.lapProgress.back().lap_distance_m < trackLengthM_)) {
                        it->second.lapProgress.push_back(
                            { t, lf.last_lap_ms, (float)trackLengthM_, 2, 0, 0 });
                    }
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
            else if (tid == 4 &&
                     (loadedFormat_ == TnrdFormat::ZstdV3 || isChunkedTnrd(loadedFormat_)) &&
                     lf.lap_num == curLapNum && std::isfinite(lf.lap_distance_m) &&
                     lf.lap_distance_m >= 0.0f) {
                it->second.lapProgress.push_back(
                    { t, lf.current_lap_ms, lf.lap_distance_m, lf.sector, lf.s1_ms, lf.s2_ms });
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
    if (memoryFile) {
        while (memoryPos < memoryFile->size()) {
            const size_t n = std::min(CHUNK, memoryFile->size() - memoryPos);
            consumeChunk(memoryFile->data() + memoryPos, n, (std::int64_t)memoryPos);
            memoryPos += n;
        }
    } else for (;;) {
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
    const bool readOk = memoryFile || std::ferror(f) == 0;
    if (f) std::fclose(f);
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

bool TnrdReader::load(const std::string& path, HeaderRow& outHeader) {
    lastError_.clear();
    std::string detectError;
    const TnrdFormat format = detail::detectTnrdFormat(path, &detectError);
    if (format == TnrdFormat::Unknown) {
        lastError_ = detectError.empty() ? "The file format could not be identified." : detectError;
        close();
        std::fprintf(stderr, "[tnrd] load FAILED: %s for '%s'\n",
                     lastError_.c_str(), path.c_str());
        return false;
    }
    return loadWithFormat(path, outHeader, format);
}

bool TnrdReader::loadZstd(const std::string& path, HeaderRow& outHeader) {
    return loadWithFormat(path, outHeader, TnrdFormat::ZstdV2);
}

bool TnrdReader::loadGzip(const std::string& path, HeaderRow& outHeader) {
    return loadWithFormat(path, outHeader, TnrdFormat::GzipV1);
}

bool TnrdReader::loadWithFormat(const std::string& path, HeaderRow& outHeader,
                                TnrdFormat format) {
    close();
    lastError_.clear();
    std::string detectError;
    const TnrdFormat detected = detail::detectTnrdFormat(path, &detectError);
    const bool codecMatches = detected == format ||
        (isLegacyZstdStream(detected) && isLegacyZstdStream(format));
    if (!codecMatches) {
        lastError_ = detected == TnrdFormat::Unknown
            ? (detectError.empty() ? "The file format could not be identified." : detectError)
            : std::string("Expected ") + toString(format) + " but found " + toString(detected) + ".";
        std::fprintf(stderr, "[tnrd] load FAILED: requested %s but file is %s ('%s')\n",
                     toString(format), toString(detected), path.c_str());
        return false;
    }
    std::fprintf(stderr, "[tnrd] load: '%s' (%s)\n", path.c_str(), toString(detected));
    if (isChunkedTnrd(detected)) {
        HeaderRow indexedHeader;
        std::unique_ptr<detail::TnrdIndexedArchive> archive;
        if (detected == TnrdFormat::ChunkedV6) {
            detail::V6LoadResult loaded;
            if (!detail::TNRD_V6::load(path, loaded, lastError_)) {
                close();
                return false;
            }
            indexedHeader = std::move(loaded.header);
            archive = std::move(loaded.archive);
        } else if (detected == TnrdFormat::ChunkedV5) {
            detail::V5LoadResult loaded;
            if (!detail::TNRD_V5::load(path, loaded, lastError_)) {
                close();
                return false;
            }
            indexedHeader = std::move(loaded.header);
            archive = std::move(loaded.archive);
        } else {
            detail::V4LoadResult loaded;
            if (!detail::TNRD_V4::load(path, loaded, lastError_)) {
                close();
                return false;
            }
            indexedHeader = std::move(loaded.header);
            archive = std::move(loaded.archive);
        }
        outHeader = std::move(indexedHeader);
        indexedArchive_ = std::move(archive);
        loadedFormat_ = detected;
        trackLengthM_ = outHeader.track_length_m.value_or(0);
        startTime_ = indexedArchive_->startTime();
        totalTime_ = indexedArchive_->totalTime();
        initialFuelKg_ = indexedArchive_->summary().initialFuelKg;
        if (detected == TnrdFormat::ChunkedV6) {
            if (auto* v6 = dynamic_cast<detail::TnrdV6Archive*>(indexedArchive_.get())) {
                if (const auto player = v6->playerDriverIndex()) {
                    recordedDriverIndex_ = *player;
                    playbackDriverIndex_ = *player;
                    playbackDriverUsesRecordedRows_ = true;
                    v6->setPlaybackDriver(*player);
                }
                v6SharedOrder_.clear();
                for (size_t i = 0; i < v6->sharedRecords().size(); ++i)
                    if (v6->sharedRecords()[i].phase == detail::V6Phase::Race)
                        v6SharedOrder_.push_back(i);
                std::stable_sort(v6SharedOrder_.begin(), v6SharedOrder_.end(),
                    [&](size_t left, size_t right) {
                        const auto& a = v6->sharedRecords()[left];
                        const auto& b = v6->sharedRecords()[right];
                        return v6->logicalTime(a.phase, a.sessionTime) <
                               v6->logicalTime(b.phase, b.sessionTime);
                    });
            }
        }
        scannedEvents_.clear();
        scannedEvents_.reserve(indexedArchive_->summary().events.size());
        for (std::string event : indexedArchive_->summary().events) {
            while (!event.empty() && (event.back() == '\n' || event.back() == '\r'))
                event.pop_back();
            glz::generic parsedEvent;
            if (event.empty() || glz::read_json(parsedEvent, event)) {
                lastError_ = "The indexed recording contains an invalid race-event summary.";
                close();
                return false;
            }
            scannedEvents_.push_back(std::move(event));
        }
        (void)rebuildV6LapCatalog();
        for (const auto& s : indexedArchive_->summary().lapStatus) {
            auto it=lapBlocks_.find((int)s.lapNumber);if(it!=lapBlocks_.end())it->second.slimStatus.push_back({"status",s.sessionTime,s.ersPct,s.tyreCompound,s.visualCompound});
        }
        strategyProtocol_ = static_cast<uint16_t>(outHeader.protocol >= 2024 ? outHeader.protocol : 2025);
        if (!buildSectorDistanceMetadata()) {
            std::fprintf(stderr, "[tnrd] load FAILED: sector metadata scan failed for '%s': %s\n",
                         path.c_str(), lastError_.c_str());
            close();
            return false;
        }

        size_t checkpointCount = 0;
        if (detected == TnrdFormat::ChunkedV4) {
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
                close();
                return false;
            }
            checkpointCount = strategyCheckpoints_.size();
            indexedArchive_->releaseTransientMemory();
        }
        setCursor(startTime_);
        if (detected == TnrdFormat::ChunkedV4)
            std::fprintf(stderr,"[tnrd] load OK: format=%s chunks=%zu laps=%zu strategyCheckpoints=%zu cacheBytes=%zu start=%.2f total=%.2f track='%s' session='%s'\n",toString(loadedFormat_),indexedArchive_->chunks().size(),indexedArchive_->laps().size(),checkpointCount,indexedArchive_->cacheBytes(),startTime_,totalTime_,outHeader.track_name.c_str(),outHeader.session_name.c_str());
        else
            std::fprintf(stderr,"[tnrd] load OK: format=%s chunks=%zu laps=%zu start=%.2f total=%.2f track='%s' session='%s'\n",toString(loadedFormat_),indexedArchive_->chunks().size(),indexedArchive_->laps().size(),startTime_,totalTime_,outHeader.track_name.c_str(),outHeader.session_name.c_str());
        return true;
    }
    bool loadedOk = false;
    bool partial = false;
    auto acceptLoaded = [&](auto&& loaded) {
        outHeader = std::move(loaded.header);
        tempPath_ = std::move(loaded.tempPath);
        partial = loaded.partial;
    };
    switch (detected) {
        case TnrdFormat::GzipV1: {
            detail::TNRD_V1::LoadResult loaded;
            loadedOk = detail::TNRD_V1::load(path, loaded, lastError_);
            if (loadedOk) acceptLoaded(std::move(loaded));
            break;
        }
        case TnrdFormat::ZstdV2: {
            detail::TNRD_V2::LoadResult loaded;
            loadedOk = detail::TNRD_V2::load(path, loaded, lastError_);
            if (loadedOk) acceptLoaded(std::move(loaded));
            break;
        }
        case TnrdFormat::ZstdV3: {
            detail::TNRD_V3::LoadResult loaded;
            loadedOk = detail::TNRD_V3::load(path, loaded, lastError_);
            if (loadedOk) acceptLoaded(std::move(loaded));
            break;
        }
        default:
            lastError_ = "The recording format has no reader implementation.";
            break;
    }
    if (!loadedOk) {
        std::fprintf(stderr, "[tnrd] load FAILED: %s for '%s'\n",
                     lastError_.c_str(), path.c_str());
        close();
        return false;
    }
    loadedFormat_ = detected;
    trackLengthM_ = outHeader.track_length_m.value_or(0);
    if (partial)
        std::fprintf(stderr, "[tnrd] load: recovered a truncated %s stream; final partial row will be dropped\n",
                     toString(detected));

    if (!buildIndex(tempPath_)) {
        lastError_ = "The decompressed recording could not be indexed.";
        std::fprintf(stderr, "[tnrd] load FAILED: 64-bit index read failed for '%s'\n", path.c_str());
        close();
        return false;
    }
    if (index_.empty()) {
        lastError_ = "The recording contains no readable telemetry rows.";
        std::fprintf(stderr, "[tnrd] load FAILED: index empty (no indexable rows) for '%s'\n", path.c_str());
        close();
        return false;
    }
    if (!buildSectorDistanceMetadata()) {
        std::fprintf(stderr, "[tnrd] load FAILED: sector metadata scan failed for '%s': %s\n",
                     path.c_str(), lastError_.c_str());
        close();
        return false;
    }

    tempFile_ = detail::openTnrdFile(tempPath_, "rb");
    if (!tempFile_) {
        lastError_ = "The decompressed recording could not be opened for playback.";
        std::fprintf(stderr, "[tnrd] load FAILED: final reopen of temp '%s' failed\n", tempPath_.c_str());
        close();
        return false;
    }
    if (!seekFile(tempFile_, 0, SEEK_END) || (tempFileSize_ = tellFile(tempFile_)) < 0) {
        lastError_ = "The decompressed recording size could not be read.";
        std::fprintf(stderr, "[tnrd] load FAILED: 64-bit size read failed for '%s'\n", path.c_str());
        close();
        return false;
    }
    strategyProtocol_ = static_cast<uint16_t>(outHeader.protocol >= 2024 ? outHeader.protocol : 2025);
    std::fprintf(stderr, "[tnrd] load OK: format=%s rows=%zu start=%.2f total=%.2f track='%s' session='%s'\n",
                 toString(loadedFormat_), index_.size(), startTime_, totalTime_,
                 outHeader.track_name.c_str(), outHeader.session_name.c_str());
    return true;
}

void TnrdReader::close() {
    std::fprintf(stderr, "[close-trace] TnrdReader::close entry format=%s index=%zu laps=%zu hotBytes=%zu seekCacheBytes=%zu temp=%s\n",
                 toString(loadedFormat_), index_.size(), lapBlocks_.size(),
                 hotBin_ ? hotBin_->size() : 0, packedSeekCacheBytes_,
                 tempPath_.empty() ? "none" : tempPath_.c_str());
    std::fflush(stderr);
    if (tempFile_) { std::fclose(tempFile_); tempFile_ = nullptr; }
    std::fprintf(stderr, "[close-trace] temp FILE handle closed\n");
    std::fflush(stderr);
    if (!tempPath_.empty()) {
        std::error_code ec;
        std::filesystem::remove(tempPath_, ec);
        std::fprintf(stderr, "[close-trace] temp path remove returned error=%s\n",
                     ec ? ec.message().c_str() : "none");
        std::fflush(stderr);
        tempPath_.clear();
    }
    loadedFormat_ = TnrdFormat::Unknown;
    playbackDriverIndex_ = -1;
    recordedDriverIndex_ = -1;
    playbackDriverUsesRecordedRows_ = false;
    playbackV6Types_.clear();
    playbackV6HistoryTypes_.clear();
    v6SharedOrder_.clear();
    v6SharedPos_ = 0;
    v6ProjectionState_ = {};
    playbackRowMask_ = playbackOutputRowMask_;
    std::fprintf(stderr, "[close-trace] resetting indexed archive\n");
    std::fflush(stderr);
    indexedArchive_.reset();
    std::fprintf(stderr, "[close-trace] indexed archive reset; retiring bounded seek caches\n");
    std::fflush(stderr);

    // Seek buffers can still be moderately large after an All Laps request.
    // Retire them away from the caller so closing playback remains responsive.
    auto retiredSeekCache = std::move(packedSeekCache_);
    auto retiredSeekLru = std::move(packedSeekLru_);
    auto retiredPlaybackLanes = std::move(v4PlaybackLanes_);
    std::thread([
        seekCache = std::move(retiredSeekCache),
        seekLru = std::move(retiredSeekLru),
        playbackLanes = std::move(retiredPlaybackLanes)
    ]() mutable {
        std::fprintf(stderr, "[close-trace] background indexed-cache release started\n");
        std::fflush(stderr);
        seekCache.clear();
        seekLru.clear();
        playbackLanes = {};
        std::fprintf(stderr, "[close-trace] background indexed-cache release complete\n");
        std::fflush(stderr);
    }).detach();
    packedSeekCacheBytes_ = 0;
    v4PlaybackLap_ = 0;
    v4PlaybackCursor_ = 0.0f;
    v4PlaybackPrepared_ = false;
    v4PlaybackPrefetchOutstanding_ = false;
    v4PlaybackDamageState_ = {};
    v4PlaybackDamageStateReady_ = false;
    index_.clear();
    std::fprintf(stderr, "[close-trace] indexed histories/caches cleared; clearing lap and strategy state\n");
    std::fflush(stderr);
    lapBlocks_.clear();
    scannedLaps_.clear();
    scannedEvents_.clear();
    strategyCheckpoints_.clear();
    strategyProtocol_ = 2025;
    // Outstanding zero-copy seek buffers retain the old immutable store until
    // Electron has serialized them; the reader immediately releases its copy.
    hotBin_ = std::make_shared<std::vector<uint8_t>>();
    std::fprintf(stderr, "[close-trace] hot binary store released; clearing cold/legacy stores\n");
    std::fflush(stderr);
    hotTimes_.clear();
    hotStart_.clear();
    hotCum_.clear();
    coldStatus_.clear();
    coldDamage_.clear();
    coldLap_.clear();
    legacyStrategyRows_.clear();
    scratch_.clear();
    std::fprintf(stderr, "[close-trace] stores cleared; shrinking scratch capacity=%zu\n", scratch_.capacity());
    std::fflush(stderr);
    scratch_.shrink_to_fit();
    fastestLapNum_ = 0;
    fastestLapMs_  = 0;
    initialFuelKg_ = -1.0;
    trackLengthM_ = 0;
    startTime_ = totalTime_ = 0.0f;
    tempFileSize_ = 0;
    playPos_ = 0;
    damageCadenceCursor_ = 0.0f;
    std::fprintf(stderr, "[close-trace] TnrdReader::close complete\n");
    std::fflush(stderr);
}

size_t TnrdReader::upperBoundTime(float t) const {
    size_t lo = 0, hi = index_.size();
    while (lo < hi) { size_t mid = (lo + hi) / 2; if (index_[mid].sessionTime <= t) lo = mid + 1; else hi = mid; }
    return lo;
}

size_t TnrdReader::lowerBoundTime(float t) const {
    size_t lo = 0, hi = index_.size();
    while (lo < hi) { size_t mid = (lo + hi) / 2; if (index_[mid].sessionTime < t) lo = mid + 1; else hi = mid; }
    return lo;
}

// Returns the raw JSONL line at the given file offset (no JSON parse).
std::string TnrdReader::readLine(FileOffset offset) {
    if (isChunkedTnrd(loadedFormat_)) return {};
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

void TnrdReader::setCursor(float t) {
    if (isChunkedTnrd(loadedFormat_)) {
        v4PlaybackLap_ = indexedArchive_ ? indexedArchive_->lapAt(t) : 0;
        v4PlaybackCursor_ = t;
        if(indexedArchive_)indexedArchive_->cancelPrefetch();
        v4PlaybackPrepared_ = false;
        v4PlaybackPrefetchOutstanding_ = false;
        if(!v4PlaybackDamageStateReady_||v4PlaybackDamageState_.t>t){v4PlaybackDamageState_={};v4PlaybackDamageStateReady_=false;}
        if (auto* v6 = dynamic_cast<detail::TnrdV6Archive*>(indexedArchive_.get())) {
            const auto& shared = v6->sharedRecords();
            v6SharedPos_ = static_cast<size_t>(std::upper_bound(
                v6SharedOrder_.begin(), v6SharedOrder_.end(), t,
                [&](float value, size_t index) {
                    return value < v6->logicalTime(shared[index].phase, shared[index].sessionTime);
                }) - v6SharedOrder_.begin());
        }
        damageCadenceCursor_=t;return;
    }
    playPos_ = upperBoundTime(t);
    damageCadenceCursor_ = t;
}

void TnrdReader::beginCursorPrime(float t) {
    setCursor(t);
    if (!hasExactTnrdIndex(loadedFormat_) || !indexedArchive_) return;

    // V5/V6 directories give us the exact target lap and per-family chunk list.
    // Queue the first playback frontier before the seek-prefix query starts so
    // both operations share the archive executor and decompressed-chunk cache.
    // prepareV4PlaybackLap() is retained as the common V4/V5 lane setup; only
    // V5 has the exact metadata and multi-worker executor needed for overlap.
    prepareV4PlaybackLap();
    prefetchV4PlaybackChunk();
}

std::vector<std::string> TnrdReader::damageRowsAtCadence(
    float fromTime, float toTime, bool includeFrom,
    const std::function<bool()>& cancelled) const {
    std::vector<std::string> out;
    if(cancelled&&cancelled())return out;
    std::vector<TimedRaw> v4Damage;
    const std::vector<TimedRaw>* source=&coldDamage_;
    if (isChunkedTnrd(loadedFormat_) && indexedArchive_) {
        std::vector<detail::V4TimedRow> rows, initial;
        std::string error;
        auto* archive = const_cast<detail::TnrdIndexedArchive*>(indexedArchive_.get());
        if (!archive->latestRows(fromTime, {3}, initial, &error, cancelled) ||
            !archive->rowsForRange(fromTime, toTime, detail::v4TypeBit(3),
                                   rows, &error, cancelled)) return out;
        if (!initial.empty())
            v4Damage.push_back({initial.back().sessionTime, std::move(initial.back().json)});
        for (auto& row : rows)
            if (v4Damage.empty() || row.sessionTime != v4Damage.back().t ||
                row.json != v4Damage.back().json)
                v4Damage.push_back({row.sessionTime, std::move(row.json)});
        source = &v4Damage;
    }
    const auto& damage=*source;
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

bool TnrdReader::rebuildV6LapCatalog() {
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
        if (loadedFormat_ == TnrdFormat::ChunkedV6) {
            // A display-number collision can occur after a garage restart. Keep
            // the latest interval, matching the active timeline used by lapAt().
            lapBlocks_.insert_or_assign(block.lapNum, std::move(block));
        } else {
            lapBlocks_.emplace(block.lapNum, std::move(block));
        }
        if (loadedFormat_ == TnrdFormat::ChunkedV6 && lap.lapNumber == 0) continue;
        scannedLaps_.push_back({static_cast<int>(lap.lapNumber), lap.startSessionTime,
                                lap.endSessionTime, static_cast<int>(lap.lapTimeMs)});
        if (lap.lapTimeMs && (!fastestLapMs_ || static_cast<int>(lap.lapTimeMs) < fastestLapMs_)) {
            fastestLapMs_ = static_cast<int>(lap.lapTimeMs);
            fastestLapNum_ = static_cast<int>(lap.lapNumber);
        }
    }
    if (loadedFormat_ == TnrdFormat::ChunkedV6 && playbackDriverIndex_ >= 0) {
        const auto* v6 = dynamic_cast<detail::TnrdV6Archive*>(indexedArchive_.get());
        const auto* driver = v6 ? v6->driverHeader(static_cast<uint8_t>(playbackDriverIndex_)) : nullptr;
        if (driver) {
            int stintStartLap = 1;
            for (const auto& stint : driver->tyreStints) {
                const int stintEndLap = stint.endLap == 255
                    ? std::numeric_limits<int>::max()
                    : stint.endLap;
                for (auto& [lapNum, block] : lapBlocks_) {
                    if (lapNum < stintStartLap || lapNum > stintEndLap ||
                        stint.actualCompound <= 0) continue;
                    block.slimStatus.push_back({
                        "status", block.endSessionTime, 0.0,
                        stint.actualCompound, stint.visualCompound,
                    });
                }
                if (stintEndLap == std::numeric_limits<int>::max()) break;
                stintStartLap = stintEndLap + 1;
            }
        }
    }
    return true;
}

bool TnrdReader::buildSectorDistanceMetadata() {
    for (auto& [_, block] : lapBlocks_) {
        block.sector1EndDistanceM = 0.0f;
        block.sector2EndDistanceM = 0.0f;
    }
    if (loadedFormat_ != TnrdFormat::ZstdV3 && !isChunkedTnrd(loadedFormat_)) return true;

    if (!isChunkedTnrd(loadedFormat_)) {
        for (auto& [_, block] : lapBlocks_) {
            const auto [sector1, sector2] = sectorEndDistances(block.lapProgress);
            block.sector1EndDistanceM = sector1;
            block.sector2EndDistanceM = sector2;
        }
        return true;
    }

    if (loadedFormat_ == TnrdFormat::ChunkedV6) {
        auto* v6 = dynamic_cast<detail::TnrdV6Archive*>(indexedArchive_.get());
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

void TnrdReader::primeCursor() {
    if (!isChunkedTnrd(loadedFormat_) || !indexedArchive_) return;
    // Seek/history extraction has normally populated time bounds and the
    // raw-chunk cache for the target. Prepare that frontier while the engine's
    // seek generation is still gated, instead of charging it to the first
    // 16 ms playback tick after the target snapshot is visible.
    (void)pullUntil(v4PlaybackCursor_);
}

// Latest row of each requested type at or before t. Walks the index backward and
// reads only the matched lines (never the whole window), returning them ordered by
// file position. Stops as soon as every requested type has been found.
std::vector<std::pair<uint8_t, std::string>> TnrdReader::latestOfTypesTagged(
    float t, const std::vector<uint8_t>& types,
    const std::function<bool()>& cancelled) {
    std::vector<std::pair<uint8_t, std::string>> out;
    if(cancelled&&cancelled())return out;
    if (isChunkedTnrd(loadedFormat_) && indexedArchive_) {
        std::vector<uint8_t> sourceTypes = types;
        uint32_t requestedMask = 0;
        for (const auto type : types) requestedMask |= detail::v4TypeBit(type);
        if (loadedFormat_ == TnrdFormat::ChunkedV6 && playbackDriverIndex_ >= 0) {
            auto* v6 = dynamic_cast<detail::TnrdV6Archive*>(indexedArchive_.get());
            if (!v6) return {};
            auto allSources = v6TypesForRowMask(requestedMask);
            std::erase_if(allSources, [&](uint8_t type) { return !v6->requestedType(type); });
            std::vector<uint8_t> multiSources;
            if (requestedMask & detail::v4TypeBit(7)) multiSources.push_back(24);
            if (requestedMask & detail::v4TypeBit(13)) multiSources.push_back(23);
            if (requestedMask & detail::v4TypeBit(9))
                multiSources.insert(multiSources.end(), {7,13,15,16,17,18,19,20});
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
                    int stintStartLap = 1;
                    const detail::V6TyreStintSummary* currentStint = nullptr;
                    for (const auto& stint : driver->tyreStints) {
                        const int stintEndLap = stint.endLap == 255
                            ? std::numeric_limits<int>::max()
                            : stint.endLap;
                        if (selectedLap >= stintStartLap && selectedLap <= stintEndLap) {
                            currentStint = &stint;
                            break;
                        }
                        if (stintEndLap == std::numeric_limits<int>::max()) break;
                        stintStartLap = stintEndLap + 1;
                    }
                    if (currentStint && currentStint->actualCompound > 0) {
                        std::string status =
                            "{\"type\":\"status\",\"_v6_type\":13,\"player_idx\":" +
                            std::to_string(playbackDriverIndex_) +
                            ",\"session_time\":" + std::to_string(selectedTime) +
                            ",\"tyre_compound\":" + std::to_string(currentStint->actualCompound) +
                            ",\"visual_compound\":" + std::to_string(currentStint->visualCompound) +
                            ",\"tyre_age_laps\":" +
                            std::to_string(std::max(0, selectedLap - stintStartLap)) + "}";
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
                        int stintStartLap = 1;
                        const detail::V6TyreStintSummary* currentStint = nullptr;
                        for (const auto& stint : driver->tyreStints) {
                            const int stintEndLap = stint.endLap == 255
                                ? std::numeric_limits<int>::max()
                                : stint.endLap;
                            if (timingCar.lap_num >= stintStartLap && timingCar.lap_num <= stintEndLap) {
                                currentStint = &stint;
                                break;
                            }
                            if (stintEndLap == std::numeric_limits<int>::max()) break;
                            stintStartLap = stintEndLap + 1;
                        }
                        if (!currentStint || currentStint->actualCompound <= 0) continue;
                        AllStatusRow status{};
                        status.session_time = projected.sessionTime;
                        status.cars.push_back({});
                        auto& statusCar = status.cars.back();
                        statusCar.idx = timingCar.idx;
                        statusCar.tyre_compound = currentStint->actualCompound;
                        statusCar.visual_compound = currentStint->visualCompound;
                        statusCar.tyre_age_laps = std::max(0, timingCar.lap_num - stintStartLap);
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
        if (loadedFormat_ == TnrdFormat::ChunkedV6 && playbackDriverIndex_ >= 0) {
            sourceTypes = v6TypesForRowMask(requestedMask);
        }
        std::vector<detail::V4TimedRow> rows;
        std::string error;
        if (!indexedArchive_->latestRows(t, sourceTypes, rows, &error, cancelled)) {
            if (!cancelled || !cancelled()) lastError_ = error;
            return {};
        }
        if (loadedFormat_ == TnrdFormat::ChunkedV6 && playbackDriverIndex_ >= 0) {
            const uint32_t savedOutputMask = playbackOutputRowMask_;
            playbackOutputRowMask_ = requestedMask;
            V6ProjectionAssembler assembler;
            for (auto& row : rows) {
                std::vector<std::pair<uint8_t, std::string>> projected;
                projectV6Row(row.rowType, row.sessionTime, row.json, projected);
                for (auto& [type, json] : projected) {
                    if (sparseV6Playback_) out.emplace_back(type, std::move(json));
                    else assembler.add(type, row.sessionTime, std::move(json));
                }
            }
            if (!sparseV6Playback_)
                for (auto& row : assembler.take())
                    out.emplace_back(row.type, std::move(row.json));
            playbackOutputRowMask_ = savedOutputMask;
        } else {
            for (auto& row : rows) out.emplace_back(row.rowType, std::move(row.json));
        }
        std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
            return scanSessionTime(a.second.data(), static_cast<int>(a.second.size())) <
                   scanSessionTime(b.second.data(), static_cast<int>(b.second.size()));
        });
        return out;
    }
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

std::vector<std::string> TnrdReader::latestOfTypes(float t, const std::vector<uint8_t>& types,
                                                   const std::function<bool()>& cancelled) {
    std::vector<std::string> out;
    for (auto& [tid, line] : latestOfTypesTagged(t, types, cancelled)) {
        (void)tid;
        out.push_back(std::move(line));
    }
    return out;
}

std::vector<std::string> TnrdReader::stateSnapshot(float t,const std::function<bool()>& cancelled) {
    return latestOfTypes(t, { std::begin(STATE_TYPE_IDS), std::end(STATE_TYPE_IDS) },cancelled);
}

std::vector<std::string> TnrdReader::readRange(float fromTime, float toTime) {
    std::vector<std::string> out;
    if(isChunkedTnrd(loadedFormat_)&&indexedArchive_){std::vector<detail::V4TimedRow> rows;std::string error;if(!indexedArchive_->rowsForRange(fromTime,toTime,0xFFFFFFFFu,rows,&error)){lastError_=error;return out;}for(auto&r:rows)out.push_back(std::move(r.json));return out;}
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

bool TnrdReader::forEachIndexedRange(
    float fromTime, float toTime, uint32_t rowTypeMask,
    const std::function<bool(const detail::V4TimedRow&)>& callback,
    const std::function<bool()>& cancelled) {
    if (!indexedArchive_ || !isChunkedTnrd(loadedFormat_) ||
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
        std::vector<detail::V4TimedRow> rows;
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

void TnrdReader::setStrategyMinimumStops(int stops) {
    const int clamped=std::clamp(stops,0,8);
    if(strategyMinimumStops_==clamped)return;
    strategyMinimumStops_=clamped;
    strategyCheckpoints_.clear();
}

void TnrdReader::setTeamColorOverrides(TeamColorOverrides overrides) {
    TeamColorOverrides sanitized = sanitizeTeamColorOverrides(overrides);
    if (teamColorOverrides_ == sanitized) return;
    teamColorOverrides_ = std::move(sanitized);
    strategyCheckpoints_.clear();
}

StrategySnapshotRow TnrdReader::strategySnapshotAt(float t, StrategyProcessor* restoredProcessor,
                                                     const std::function<bool()>& cancelled) {
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

bool TnrdReader::forEachStrategyRow(
    float fromTime, float toTime, bool includeFrom,
    const std::function<void(float, std::string_view)>& callback,
    const std::function<bool()>& cancelled) {
    if(cancelled&&cancelled())return false;
    if (toTime < fromTime) return true;
    if (isChunkedTnrd(loadedFormat_) && indexedArchive_) {
        if (hasExactTnrdIndex(loadedFormat_)) {
            std::vector<detail::V4TimedRow> rows;
            std::string error;
            if (!indexedArchive_->rowsForRange(fromTime, toTime,
                                               kStrategyDependencyMask,
                                               rows, &error, cancelled)) {
                if (!cancelled || !cancelled()) lastError_ = std::move(error);
                return false;
            }
            // V6 keeps a driver's state as per-field families, not as the
            // composite rows the strategy reducer parses: handing them over raw
            // leaves it without lap, status and damage, so it never leaves its
            // waiting state. Project them the way the playback cursor does, and
            // re-merge each timestamp's families into one row per legacy type —
            // the reducer replaces its stored row wholesale, so a sparse patch
            // would blank the fields it does not carry. Shared V6 records and
            // every V5 row are already composite and pass straight through.
            const bool projectV6 = loadedFormat_ == TnrdFormat::ChunkedV6;
            V6ProjectionAssembler assembler;
            bool assembling = false;
            float assembledTime = 0.0f;
            const auto drain = [&] {
                assembling = false;
                for (const auto& record : assembler.take())
                    callback(record.sessionTime, record.json);
            };
            for (const auto& row : rows) {
                if (cancelled && cancelled()) return false;
                if (!includeFrom && row.sessionTime <= fromTime) continue;
                // Range reads return family rows untagged: only the playback
                // cursor adds the _v6_type marker. A family row is prefixed with
                // its driver_idx and its rowType is the V6 family; a shared
                // record is a plain legacy row whose rowType is its legacy type.
                const bool familyRow = projectV6 &&
                    std::string_view(row.json).substr(0, 14) == "{\"driver_idx\":";
                const uint8_t storedType = familyRow ? row.rowType : 0;
                if (storedType != 0) {
                    std::vector<std::pair<uint8_t, std::string>> projected;
                    projectV6Row(storedType, row.sessionTime, row.json, projected,
                                 -1, kStrategyDependencyMask);
                    if (projected.empty()) continue;
                    if (assembling && row.sessionTime != assembledTime) drain();
                    assembling = true;
                    assembledTime = row.sessionTime;
                    for (auto& [type, json] : projected)
                        assembler.add(type, row.sessionTime, std::move(json));
                    continue;
                }
                if (assembling) drain();
                callback(row.sessionTime, row.json);
            }
            if (assembling) drain();
            return true;
        }
        return forEachIndexedRange(fromTime, toTime, kStrategyDependencyMask,
            [&](const detail::V4TimedRow& row) {
                if (!includeFrom && row.sessionTime <= fromTime) return true;
                callback(row.sessionTime, row.json);
                return true;
            }, cancelled);
    }
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

void TnrdReader::setPlaybackRowMask(uint32_t mask, float cursorTime) {
    if (playbackOutputRowMask_ == mask) return;
    playbackOutputRowMask_ = mask;
    playbackRowMask_ = expandedPlaybackMask(mask);
    if (isChunkedTnrd(loadedFormat_) && indexedArchive_) setCursor(cursorTime);
}

void TnrdReader::setPlaybackDriver(int driverIndex, bool useRecordedRows, float cursorTime) {
    (void)useRecordedRows;
    const int next = loadedFormat_ == TnrdFormat::ChunkedV6
        ? (driverIndex >= 0 ? driverIndex : recordedDriverIndex_) : -1;
    const bool nextUsesRecordedRows = loadedFormat_ == TnrdFormat::ChunkedV6 && next >= 0;
    if (playbackDriverIndex_ == next &&
        playbackDriverUsesRecordedRows_ == nextUsesRecordedRows) return;
    auto* v6 = dynamic_cast<detail::TnrdV6Archive*>(indexedArchive_.get());
    if (!v6 || next < 0 || !v6->driverHeader(static_cast<uint8_t>(next))) return;
    playbackDriverIndex_ = next;
    playbackDriverUsesRecordedRows_ = nextUsesRecordedRows;
    if (v6) {
        v6->setPlaybackDriver(static_cast<uint8_t>(next));
        startTime_ = v6->startTime();
        totalTime_ = v6->totalTime();
        (void)rebuildV6LapCatalog();
        strategyCheckpoints_.clear();
        (void)buildSectorDistanceMetadata();
    }
    playbackRowMask_ = expandedPlaybackMask(playbackOutputRowMask_);
    v4PlaybackDamageState_ = {};
    v4PlaybackDamageStateReady_ = false;
    packedSeekCache_.clear();
    packedSeekLru_.clear();
    packedSeekCacheBytes_ = 0;
    if (isChunkedTnrd(loadedFormat_) && indexedArchive_) setCursor(cursorTime);
}

uint32_t TnrdReader::expandedPlaybackMask(uint32_t outputMask) const {
    return outputMask;
}

void TnrdReader::setPlaybackV6Types(const std::vector<uint8_t>& streamTypes,
                                    const std::vector<uint8_t>& historyTypes,
                                    float cursorTime) {
    playbackV6Types_ = streamTypes;
    playbackV6HistoryTypes_ = historyTypes;
    if (auto* v6 = dynamic_cast<detail::TnrdV6Archive*>(indexedArchive_.get())) {
        v6->setRequestedTypes(streamTypes); setCursor(cursorTime);
    }
}

void TnrdReader::projectV6Row(
    uint8_t sourceType, float sessionTime, std::string_view json,
    std::vector<std::pair<uint8_t, std::string>>& out,
    int selectedDriverOverride, uint32_t outputMaskOverride) const {
    const uint32_t outputMask = outputMaskOverride != 0
        ? outputMaskOverride : playbackOutputRowMask_;
    const auto wants = [&](uint8_t type) {
        return (outputMask & detail::v4TypeBit(type)) != 0;
    };
    if (loadedFormat_ != TnrdFormat::ChunkedV6 || sourceType == 0 || sourceType > 24) return;
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

bool TnrdReader::currentLapAt(float t, float& startOut, int& numOut) const {
    if (isChunkedTnrd(loadedFormat_) && indexedArchive_) {
        if (loadedFormat_ == TnrdFormat::ChunkedV6 && playbackDriverIndex_ >= 0 &&
            !playbackDriverUsesRecordedRows_) {
            // V6's persisted lap directory follows the recorded player. For a
            // selected driver, derive their own lap origin from the latest
            // all-car LapData sample instead: currentLapTimeInMS is measured
            // from that driver's most recent timing-line crossing.
            std::vector<detail::V4TimedRow> rows;
            std::string error;
            if (const_cast<detail::TnrdIndexedArchive*>(indexedArchive_.get())->latestRows(
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

std::string TnrdReader::driverRestrictionMessage(float cursorTime) const {
    if (loadedFormat_ != TnrdFormat::ChunkedV6) return {};
    const auto* v6 = dynamic_cast<const detail::TnrdV6Archive*>(indexedArchive_.get());
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

std::string TnrdReader::lapBlocksMessage() const {
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
    msg.tnrdVersion = loadedFormat_ == TnrdFormat::ChunkedV6 ? "TNRD_V6"
                    : loadedFormat_ == TnrdFormat::ChunkedV5 ? "TNRD_V5"
                    : loadedFormat_ == TnrdFormat::ChunkedV4 ? "TNRD_V4"
                    : loadedFormat_ == TnrdFormat::ZstdV3 ? "TNRD_V3"
                    : loadedFormat_ == TnrdFormat::ZstdV2 ? "TNRD_V2"
                    : loadedFormat_ == TnrdFormat::GzipV1 ? "TNRD_V1" : "";
    msg.deltaAvailable = loadedFormat_ == TnrdFormat::ZstdV3 || isChunkedTnrd(loadedFormat_);
    msg.lapDistanceAvailable = msg.deltaAvailable;
    msg.trackLengthM = msg.deltaAvailable ? trackLengthM_ : 0;
    msg.playbackDriverIndex = playbackDriverIndex_;
    if (loadedFormat_ == TnrdFormat::ChunkedV6) {
        const auto* v6 = dynamic_cast<const detail::TnrdV6Archive*>(indexedArchive_.get());
        if (v6) {
            msg.analysisDrivers.reserve(v6->driverHeaders().size());
            for (const auto& driver : v6->driverHeaders()) {
                AnalysisDriverLapCatalog catalog;
                catalog.driverIndex = driver.vehicleIndex;
                catalog.driverName = driver.driverName;
                catalog.isPlayer = driver.isPlayer;
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
                int stintStartLap = 1;
                for (const auto& stint : driver.tyreStints) {
                    const int stintEndLap = stint.endLap == 255
                        ? std::numeric_limits<int>::max() : stint.endLap;
                    for (auto& block : catalog.blocks) {
                        if (block.lapNum < stintStartLap || block.lapNum > stintEndLap ||
                            stint.actualCompound <= 0) continue;
                        block.statusHistory.push_back({
                            "status", block.endSessionTime, 0.0,
                            stint.actualCompound, stint.visualCompound,
                        });
                    }
                    if (stintEndLap == std::numeric_limits<int>::max()) break;
                    stintStartLap = stintEndLap + 1;
                }
                msg.analysisDrivers.push_back(std::move(catalog));
            }
        }
    }
    msg.events.reserve(scannedEvents_.size());
    for (const auto& s : scannedEvents_) msg.events.push_back(glz::raw_json{ s });
    for (const auto& l : scannedLaps_)   msg.laps.push_back({ l.lapNum, l.lapTimeMs });
    return writeJson(msg);
}

std::string TnrdReader::getLapDataMessage(int lapNum, uint32_t rowTypeMask,
                                          int driverIndex) const {
    if (loadedFormat_ == TnrdFormat::ChunkedV6 && driverIndex >= 0 &&
        driverIndex != playbackDriverIndex_) {
        auto* v6 = dynamic_cast<detail::TnrdV6Archive*>(indexedArchive_.get());
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
    if(isChunkedTnrd(loadedFormat_)&&indexedArchive_){
        auto* self = const_cast<TnrdReader*>(this);
        const uint32_t savedOutputMask = self->playbackOutputRowMask_;
        self->playbackOutputRowMask_ = rowTypeMask;
        const uint32_t requestedMask = loadedFormat_ == TnrdFormat::ChunkedV6 && playbackDriverIndex_ >= 0
            ? expandedPlaybackMask(rowTypeMask) : rowTypeMask;
        // An indexed lap request is self-contained. Analysis can issue it in
        // the same render commit that publishes new page requirements, so
        // consulting the previous page's V6 history subscription can return a
        // partial lap while claiming the full row-family mask.
        const std::vector<uint8_t> selectedV6Types = v6TypesForRowMask(rowTypeMask);
        auto* v6 = dynamic_cast<detail::TnrdV6Archive*>(indexedArchive_.get());
        if (v6) v6->setRequestedTypes(selectedV6Types);
        std::vector<detail::V4TimedRow> rows;std::string error;const uint32_t mask=requestedMask&(detail::v4TypeBit(1)|detail::v4TypeBit(2)|detail::v4TypeBit(3)|detail::v4TypeBit(4)|detail::v4TypeBit(11)|detail::v4TypeBit(12)|detail::v4TypeBit(13)|detail::v4TypeBit(7)|detail::v4TypeBit(8)|detail::v4TypeBit(9));
        if(!const_cast<detail::TnrdIndexedArchive*>(indexedArchive_.get())->rowsForLap((uint32_t)lapNum,mask,rows,&error)){if(v6)v6->setRequestedTypes(playbackV6Types_);self->playbackOutputRowMask_=savedOutputMask;return {};}
        if (v6) v6->setRequestedTypes(playbackV6Types_);
        std::vector<V6ProjectedRecord> outputRows;
        uint64_t outputOrder = 0;
        for(const auto&r:rows){
            if (loadedFormat_ == TnrdFormat::ChunkedV6 &&
                !selectedV6Types.empty() &&
                std::find(selectedV6Types.begin(), selectedV6Types.end(), r.rowType) == selectedV6Types.end())
                continue;
            // A timed practice/quali lap number is reused across the in-lap,
            // garage and following out-lap. Those rows can share an indexed chunk
            // key with the next flying attempt, so honour the indexed flying
            // interval when materialising a lap.
            if(r.sessionTime<b.startSessionTime||r.sessionTime>b.endSessionTime)continue;
            if (loadedFormat_ == TnrdFormat::ChunkedV6) {
                std::vector<std::pair<uint8_t, std::string>> projected;
                projectV6Row(r.rowType, r.sessionTime, r.json, projected);
                for (auto& [type, json] : projected)
                    outputRows.push_back({type, r.sessionTime, outputOrder++, std::move(json)});
            } else {
                outputRows.push_back({r.rowType, r.sessionTime, outputOrder++, r.json});
            }
        }
        if (loadedFormat_ == TnrdFormat::ChunkedV6 && !sparseV6Playback_) {
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
    auto fill = [](std::vector<glz::raw_json>& dst, const std::vector<TimedRaw>& src) {
        dst.reserve(src.size());
        for (const auto& e : src) dst.push_back(glz::raw_json{ e.json });
    };
    if(rowTypeMask&detail::v4TypeBit(1))fill(msg.telemetry, b.telemetry);
    if(rowTypeMask&detail::v4TypeBit(2))fill(msg.statusHistory, b.statusHistory);
    if(rowTypeMask&detail::v4TypeBit(11))fill(msg.motionHistory, b.motionHistory);
    if(rowTypeMask&detail::v4TypeBit(12))fill(msg.motionExHistory, b.motionExHistory);
    if(rowTypeMask&detail::v4TypeBit(3))for(const auto&row:damageRowsAtCadence(b.startSessionTime,b.endSessionTime))msg.damageHistory.push_back(glz::raw_json{row});
    if ((rowTypeMask&detail::v4TypeBit(4)) && (loadedFormat_ == TnrdFormat::ZstdV3 || isChunkedTnrd(loadedFormat_)))
        msg.lapProgress = b.lapProgress;
    if(rowTypeMask&detail::v4TypeBit(13))msg.playerPositions = b.playerPositions;
    return writeJson(msg);
}

bool TnrdReader::getAnalysisLapProgress(int lapNum, AnalysisLapProgress& out,
                                        int driverIndex) const {
    if (loadedFormat_ == TnrdFormat::ChunkedV6 && driverIndex >= 0 &&
        driverIndex != playbackDriverIndex_) {
        auto* v6 = dynamic_cast<detail::TnrdV6Archive*>(indexedArchive_.get());
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

    if (isChunkedTnrd(loadedFormat_) && indexedArchive_) {
        std::vector<detail::V4TimedRow> rows;
        std::string error;
        const bool v6 = loadedFormat_ == TnrdFormat::ChunkedV6;
        const uint8_t sourceType = v6 ? 24 : 4;
        if (!const_cast<detail::TnrdIndexedArchive*>(indexedArchive_.get())->rowsForLap(
                static_cast<uint32_t>(lapNum), detail::v4TypeBit(4), rows, &error)) return false;
        for (const auto& row : rows) {
            if (row.rowType != sourceType || row.sessionTime < block.startSessionTime ||
                row.sessionTime > block.endSessionTime) continue;
            if (v6 || sourceType == 4) {
                LapScanFields lap{};
                (void)glz::read<kPartialRead>(lap, row.json);
                result.points.push_back({row.sessionTime, lap.current_lap_ms,
                    lap.lap_distance_m, lap.sector, lap.s1_ms, lap.s2_ms});
            }
        }
    } else if (loadedFormat_ == TnrdFormat::ZstdV3) {
        result.points = block.lapProgress;
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

void TnrdReader::prepareV4PlaybackLap() {
    const float inf=std::numeric_limits<float>::infinity();
    const bool exactIndex=hasExactTnrdIndex(loadedFormat_);
    for(auto& lane:v4PlaybackLanes_){lane.chunks.clear();lane.nextChunk=0;lane.nextPrefetched=false;lane.rows.clear();lane.rowPos=0;lane.maxDecodedTime=-inf;lane.safeThrough=inf;}
    if(!indexedArchive_||!indexedArchive_->isOpen()||v4PlaybackLap_<0){v4PlaybackPrepared_=true;return;}
    const auto& chunks=indexedArchive_->chunks();std::vector<size_t> selected;
    if (auto* v6 = dynamic_cast<detail::TnrdV6Archive*>(indexedArchive_.get()))
        v6->playbackChunkIndices(playbackRowMask_, selected);
    else
        indexedArchive_->chunkIndicesForLap((uint32_t)v4PlaybackLap_,playbackRowMask_,selected);
    for(size_t i:selected){const auto& chunk=chunks[i];if(chunk.rowType<v4PlaybackLanes_.size())v4PlaybackLanes_[chunk.rowType].chunks.push_back(i);}
    for(auto& lane:v4PlaybackLanes_){
        // Range/latest-at-time extraction performed by a seek records exact
        // bounds for every chunk it inspected. Chunks wholly at/before the new
        // cursor cannot contribute a future row, so begin at the first
        // intersecting/unknown chunk rather than replaying and discarding the
        // entire lap prefix. Unknown bounds retain the conservative old path.
        while(lane.nextChunk<lane.chunks.size()){
            float first=0.0f,last=0.0f;
            if(!indexedArchive_->chunkTimeBounds(lane.chunks[lane.nextChunk],first,last)||last>v4PlaybackCursor_)break;
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
    v4PlaybackPrepared_=true;
}

bool TnrdReader::loadV4PlaybackFrontier(float throughTime) {
    struct PendingChunk{size_t lane;size_t index;uint64_t sequence;};
    const bool exactIndex=hasExactTnrdIndex(loadedFormat_);
    std::vector<PendingChunk> pending;const auto& chunks=indexedArchive_->chunks();float priority=std::numeric_limits<float>::infinity();
    for(const auto& lane:v4PlaybackLanes_)if(lane.nextChunk<lane.chunks.size()&&lane.safeThrough<=throughTime)priority=std::min(priority,lane.safeThrough);
    for(size_t i=0;i<v4PlaybackLanes_.size();++i){const auto& lane=v4PlaybackLanes_[i];if(lane.nextChunk<lane.chunks.size()&&lane.safeThrough==priority){const size_t index=lane.chunks[lane.nextChunk];pending.push_back({i,index,chunks[index].sequence});}}
    if(pending.empty())return false;
    std::sort(pending.begin(),pending.end(),[](const auto&a,const auto&b){return a.sequence<b.sequence;});
    std::vector<size_t> indices;indices.reserve(pending.size());for(const auto& item:pending)indices.push_back(item.index);
    std::vector<std::vector<detail::V4TimedRow>> decoded;std::string error;
    if(dynamic_cast<detail::TnrdV6Archive*>(indexedArchive_.get())){
        if(!indexedArchive_->rowsForChunks(indices,decoded,&error)){lastError_=error;return false;}
    }else if(auto* v5=dynamic_cast<detail::TnrdV5Archive*>(indexedArchive_.get())){
        if(!v5->rowsForChunksRange(indices,v4PlaybackCursor_,std::numeric_limits<float>::infinity(),decoded,&error)){lastError_=error;return false;}
    }else if(!indexedArchive_->rowsForChunks(indices,decoded,&error)){lastError_=error;return false;}
    // The app recorder accepts at most 200 ms of packet reordering before it
    // rewinds the indexed timeline. Hold that tail until the successor is known.
    const auto block=lapBlocks_.find(v4PlaybackLap_);const float inf=std::numeric_limits<float>::infinity();constexpr float REORDER_WINDOW_S=0.2f;
    auto before=[](const V4PlaybackRow&a,const V4PlaybackRow&b){if(a.t!=b.t)return a.t<b.t;return a.sequence<b.sequence;};
    for(size_t i=0;i<pending.size();++i){
        auto& lane=v4PlaybackLanes_[pending[i].lane];
        lane.nextPrefetched=false;
        if(lane.rowPos){lane.rows.erase(lane.rows.begin(),lane.rows.begin()+(ptrdiff_t)lane.rowPos);lane.rowPos=0;}
        const size_t retained=lane.rows.size();
        for(auto& row:decoded[i]){
            const bool inBlock=loadedFormat_==TnrdFormat::ChunkedV6||block==lapBlocks_.end()||(row.sessionTime>=block->second.startSessionTime&&row.sessionTime<=block->second.endSessionTime);
            if(!inBlock)continue;if(std::isfinite(row.sessionTime))lane.maxDecodedTime=std::max(lane.maxDecodedTime,row.sessionTime);
            if(pending[i].lane==3&&row.sessionTime<=v4PlaybackCursor_&&(!v4PlaybackDamageStateReady_||row.sessionTime>=v4PlaybackDamageState_.t)){v4PlaybackDamageState_={row.sessionTime,std::move(row.json)};v4PlaybackDamageStateReady_=true;}
            else if(row.sessionTime>v4PlaybackCursor_){if(loadedFormat_==TnrdFormat::ChunkedV6){setSessionTime(row.json,row.sessionTime);tagV6StoredType(row.json,row.rowType);}lane.rows.push_back({row.sessionTime,row.sequence,std::move(row.json)});}
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
    v4PlaybackPrefetchOutstanding_=std::any_of(v4PlaybackLanes_.begin(),v4PlaybackLanes_.end(),[](const auto& lane){return lane.nextPrefetched;});
    prefetchV4PlaybackChunk();
    return true;
}

void TnrdReader::prefetchV4PlaybackChunk() {
    if(!indexedArchive_)return;const auto& chunks=indexedArchive_->chunks();const size_t limit=hasExactTnrdIndex(loadedFormat_)?4u:1u;size_t outstanding=0;for(const auto& lane:v4PlaybackLanes_)if(lane.nextPrefetched)++outstanding;if(outstanding>=limit){v4PlaybackPrefetchOutstanding_=true;return;}
    std::vector<size_t> candidates;for(size_t i=0;i<v4PlaybackLanes_.size();++i){const auto& lane=v4PlaybackLanes_[i];if(!lane.nextPrefetched&&lane.nextChunk<lane.chunks.size())candidates.push_back(i);}std::stable_sort(candidates.begin(),candidates.end(),[&](size_t a,size_t b){const auto& left=v4PlaybackLanes_[a];const auto& right=v4PlaybackLanes_[b];if(left.safeThrough!=right.safeThrough)return left.safeThrough<right.safeThrough;return chunks[left.chunks[left.nextChunk]].sequence<chunks[right.chunks[right.nextChunk]].sequence;});
    for(size_t laneIndex:candidates){if(outstanding>=limit)break;auto& lane=v4PlaybackLanes_[laneIndex];lane.nextPrefetched=true;++outstanding;indexedArchive_->prefetchChunk(lane.chunks[lane.nextChunk]);}v4PlaybackPrefetchOutstanding_=outstanding!=0;
}

bool TnrdReader::encodeV4HotRow(uint8_t type,std::string_view json,std::vector<uint8_t>& out){
    if(type==1){TelemetryRow row{};if(glz::read<kPartialRead>(row,json))return false;if(!hasExactTnrdIndex(loadedFormat_)){row.rev_lights_pct.reset();row.rev_lights_bit_value.reset();}bin::encodeTelemetry(out,row);return true;}
    if(type==11){MotionRow row{};if(glz::read<kPartialRead>(row,json))return false;bin::encodeMotion(out,row);return true;}
    if(type==12){MotionExRow row{};if(glz::read<kPartialRead>(row,json))return false;bin::encodeMotionEx(out,row);return true;}
    return false;
}

bool TnrdReader::encodeV4HotRowCached(const detail::V4TimedRow& row,std::vector<uint8_t>& out){
    const uint64_t key=(row.sequence<<32)|row.sourceOffset;auto hit=packedSeekCache_.find(key);
    if(hit!=packedSeekCache_.end()){packedSeekLru_.splice(packedSeekLru_.begin(),packedSeekLru_,hit->second.lru);hit->second.lru=packedSeekLru_.begin();out.insert(out.end(),hit->second.bytes.begin(),hit->second.bytes.end());return true;}
    std::vector<uint8_t> encoded;if(!encodeV4HotRow(row.rowType,row.json,encoded))return false;out.insert(out.end(),encoded.begin(),encoded.end());if(encoded.size()>PACKED_SEEK_CACHE_LIMIT)return true;
    while(packedSeekCacheBytes_+encoded.size()>PACKED_SEEK_CACHE_LIMIT&&!packedSeekLru_.empty()){const uint64_t old=packedSeekLru_.back();packedSeekLru_.pop_back();auto oldEntry=packedSeekCache_.find(old);packedSeekCacheBytes_-=oldEntry->second.bytes.size();packedSeekCache_.erase(oldEntry);}packedSeekLru_.push_front(key);packedSeekCacheBytes_+=encoded.size();packedSeekCache_.emplace(key,PackedSeekCacheEntry{std::move(encoded),packedSeekLru_.begin()});return true;
}

// First index >= playPos_ whose sessionTime exceeds t. Linear (not a binary
// search) on purpose: it matches the old per-row pullUntil's early stop at the
// first out-of-order row, so an ahead-of-time row is delivered when the cursor
// actually reaches it rather than skipped.
size_t TnrdReader::pullEnd(float t) const {
    size_t end = playPos_;
    while (end < index_.size() && index_[end].sessionTime <= t) ++end;
    return end;
}

// One contiguous fread of the byte range behind index_[fromIdx..toIdx) into
// scratch_. Replaces the old per-row fseek+fgets walk — one seek + one read per
// tick instead of one per row, which dominated the playback tick cost.
size_t TnrdReader::readBlock(size_t fromIdx, size_t toIdx) {
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

// Walks the lines of a readBlock()'d range in lockstep with its index entries.
// Lines the index pass skipped (length <= 1, see commitLine) are skipped here
// too without consuming an entry; a truncated unindexed tail past the last
// entry is never reached because the walk stops after `count` entries.
// `perEntry(entryIdx, lineData, lineLen)` is invoked once per index entry.
template <class F>
static void walkBlockLines(const char* data, size_t got,
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

std::vector<std::string> TnrdReader::pullUntil(float t) {
    std::vector<std::string> out;
    if(isChunkedTnrd(loadedFormat_)&&indexedArchive_){
        if(!v4PlaybackPrepared_)prepareV4PlaybackLap();
        const float inf=std::numeric_limits<float>::infinity();
        auto before=[](const V4PlaybackRow&a,const V4PlaybackRow&b){if(a.t!=b.t)return a.t<b.t;return a.sequence<b.sequence;};
        for(;;){
            size_t best=v4PlaybackLanes_.size();float safeThrough=inf;bool futureChunk=false;
            for(size_t i=0;i<v4PlaybackLanes_.size();++i){const auto& lane=v4PlaybackLanes_[i];safeThrough=std::min(safeThrough,lane.safeThrough);futureChunk|=lane.nextChunk<lane.chunks.size();if(lane.rowPos<lane.rows.size()&&(best==v4PlaybackLanes_.size()||before(lane.rows[lane.rowPos],v4PlaybackLanes_[best].rows[v4PlaybackLanes_[best].rowPos])))best=i;}
            if(best!=v4PlaybackLanes_.size()&&v4PlaybackLanes_[best].rows[v4PlaybackLanes_[best].rowPos].t<=t&&(v4PlaybackLanes_[best].rows[v4PlaybackLanes_[best].rowPos].t<safeThrough||!futureChunk)){auto& lane=v4PlaybackLanes_[best];out.push_back(std::move(lane.rows[lane.rowPos++].json));continue;}
            const float through=best!=v4PlaybackLanes_.size()&&v4PlaybackLanes_[best].rows[v4PlaybackLanes_[best].rowPos].t<=t?v4PlaybackLanes_[best].rows[v4PlaybackLanes_[best].rowPos].t:t;
            if(futureChunk&&safeThrough<=through){if(loadV4PlaybackFrontier(through))continue;break;}
            if(best!=v4PlaybackLanes_.size())break;
            if(loadedFormat_==TnrdFormat::ChunkedV6)break;
            const auto& laps=indexedArchive_->laps();auto next=v4PlaybackLap_==0?laps.begin():std::find_if(laps.begin(),laps.end(),[&](const auto& lap){return (int)lap.lapNumber==v4PlaybackLap_;});if(v4PlaybackLap_!=0&&next!=laps.end())++next;if(next==laps.end()||next->startSessionTime>t)break;indexedArchive_->cancelPrefetch();v4PlaybackPrefetchOutstanding_=false;v4PlaybackLap_=(int)next->lapNumber;v4PlaybackPrepared_=false;prepareV4PlaybackLap();
        }
        prefetchV4PlaybackChunk();return out;
    }
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

std::vector<std::string> TnrdReader::drainRest() {
    // +INFINITY compares greater than every finite sessionTime, so pullUntil
    // walks to the end of the index — identical to the old drain loop.
    return pullUntil(INFINITY);
}

void TnrdReader::pullUntilSplit(float t, std::string& jsonOut, std::vector<uint8_t>& binOut,
                                uint32_t& seenTypes,
                                std::array<std::string, 16>* lastOfType) {
    const float cadenceEnd = std::isfinite(t) ? t : totalTime_;
    if(isChunkedTnrd(loadedFormat_)&&indexedArchive_){
        if (loadedFormat_ == TnrdFormat::ChunkedV6 && playbackDriverIndex_ >= 0) {
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
            if (auto* v6 = dynamic_cast<detail::TnrdV6Archive*>(indexedArchive_.get())) {
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
        const bool reconstructDamage = loadedFormat_ != TnrdFormat::ChunkedV6;
        if(reconstructDamage&&!v4PlaybackDamageStateReady_&&lastOfType&&!(*lastOfType)[3].empty()){const float seedTime=scanSessionTime((*lastOfType)[3].data(),(int)(*lastOfType)[3].size());if(seedTime<=damageCadenceCursor_){v4PlaybackDamageState_={seedTime,(*lastOfType)[3]};v4PlaybackDamageStateReady_=true;}}
        std::vector<TimedRaw> damageUpdates;auto rows=pullUntil(t);
        for(auto&row:rows){const uint8_t tid=scanType(row.data(),(int)row.size());if(!(playbackRowMask_&(1u<<tid)))continue;seenTypes|=(1u<<tid);if(tid==1||tid==11||tid==12){(void)encodeV4HotRow(tid,row,binOut);continue;}if(tid==3&&reconstructDamage){if(lastOfType)(*lastOfType)[3]=row;damageUpdates.push_back({scanSessionTime(row.data(),(int)row.size()),std::move(row)});continue;}jsonOut+=row;jsonOut.push_back('\n');if(lastOfType&&tid<lastOfType->size())(*lastOfType)[tid]=row;}
        if(reconstructDamage&&(playbackRowMask_&(1u<<3))&&std::isfinite(damageCadenceCursor_)&&std::isfinite(cadenceEnd)&&cadenceEnd>=damageCadenceCursor_){
            constexpr double RATE=10.0,EPS=1e-6;const long long firstTick=(long long)std::floor((double)damageCadenceCursor_*RATE+EPS)+1,lastTick=(long long)std::floor((double)cadenceEnd*RATE+EPS);size_t update=0;
            for(long long tick=firstTick;tick<=lastTick;++tick){const float sampleTime=(float)((double)tick/RATE);while(update<damageUpdates.size()&&damageUpdates[update].t<=sampleTime+(float)EPS){v4PlaybackDamageState_=std::move(damageUpdates[update++]);v4PlaybackDamageStateReady_=true;}if(!v4PlaybackDamageStateReady_)continue;std::string row=v4PlaybackDamageState_.json;setSessionTime(row,sampleTime);jsonOut+=row;jsonOut.push_back('\n');seenTypes|=(1u<<3);if(lastOfType)(*lastOfType)[3]=row;}
            while(update<damageUpdates.size()){v4PlaybackDamageState_=std::move(damageUpdates[update++]);v4PlaybackDamageStateReady_=true;}
        }
        damageCadenceCursor_=cadenceEnd;return;
    }
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

TnrdReader::SeekFlush TnrdReader::seekFlush(float target, float currentLapStart,
                                            bool allHistory, uint32_t requestedTypes,
                                            float windowSeconds, bool includeMandatoryState,
                                            const std::function<bool()>& cancelled) {
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

    if(isChunkedTnrd(loadedFormat_)&&indexedArchive_){
        const bool currentLapOnly=!allHistory&&windowSeconds<=0.0f;
        if (loadedFormat_ == TnrdFormat::ChunkedV6) {
            auto* v6 = dynamic_cast<detail::TnrdV6Archive*>(indexedArchive_.get());
            const auto& historyTypes = playbackV6HistoryTypes_.empty()
                ? playbackV6Types_ : playbackV6HistoryTypes_;
            if (v6) v6->setRequestedTypes(historyTypes);
            const uint32_t savedOutputMask = playbackOutputRowMask_;
            playbackOutputRowMask_ = mask;
            const uint32_t sourceMask = expandedPlaybackMask(mask);
            std::string error;
            V6ProjectionAssembler compatibilityAssembler;
            const auto appendProjected = [&](const detail::V4TimedRow& source,
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
                    std::vector<detail::V4TimedRow> seeds;
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
                [&](const detail::V4TimedRow& source) {
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
                        if (!encodeV4HotRow(projected.type, projected.json, *binary)) {
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
        // V1-V5 recordings deduplicated unchanged damage state, so their
        // readers reconstruct the original 10 Hz stream. V6 persists every
        // damage packet and reads it directly like every other cold family.
        const bool reconstructDamage = loadedFormat_ != TnrdFormat::ChunkedV6;
        const uint32_t directMask = reconstructDamage
            ? mask & ~detail::v4TypeBit(3) : mask;
        if(cancelled&&cancelled())return f;
        auto binary=std::make_shared<std::vector<uint8_t>>();
        const auto appendRow = [&](const detail::V4TimedRow& row) {
            if (row.rowType == 1 || row.rowType == 11 || row.rowType == 12)
                return encodeV4HotRowCached(row, *binary);
            f.coldJson += row.json;
            f.coldJson.push_back('\n');
            return true;
        };
        bool loaded = true;
        if (directMask && hasExactTnrdIndex(loadedFormat_)) {
            // V5/V6 persist exact chunk time bounds and row offsets, so preserve
            // their direct single-query seek path. Unlike V4, they do not need
            // the sliced walker to bound discovery of missing chunk metadata.
            std::vector<detail::V4TimedRow> rows;
            std::string error;
            loaded = currentLapOnly
                ? indexedArchive_->rowsForLapRange(
                    (uint32_t)indexedArchive_->lapAt(target), windowStart,
                    target, directMask, rows, &error, cancelled)
                : indexedArchive_->rowsForRange(
                    windowStart, target, directMask, rows, &error, cancelled);
            if (!loaded) {
                if (!cancelled || !cancelled()) lastError_ = std::move(error);
            } else {
                for (const auto& row : rows) {
                    if (row.rowType == 1 || row.rowType == 11 || row.rowType == 12)
                        (void)encodeV4HotRowCached(row, *binary);
                    else {
                        f.coldJson += row.json;
                        f.coldJson.push_back('\n');
                    }
                }
            }
        } else if (directMask && currentLapOnly) {
            std::vector<detail::V4TimedRow> rows;
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
        if(reconstructDamage&&(mask&detail::v4TypeBit(3))){auto damage=damageRowsAtCadence(windowStart,target,true,cancelled);if(cancelled&&cancelled())return {};if(!damage.empty()){v4PlaybackDamageState_={scanSessionTime(damage.back().data(),(int)damage.back().size()),damage.back()};v4PlaybackDamageStateReady_=true;}for(auto&row:damage){f.coldJson+=row;f.coldJson.push_back('\n');}}if(!f.coldJson.empty())f.coldJson.pop_back();if(!binary->empty()){f.binaryStore=binary;f.binaryBegin=0;f.binaryEnd=binary->size();}return f;
    }

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
        if (!(mask & detail::v4TypeBit(rowType))) return;
        for (const auto& r : rows) {
            if (r.t > target || r.t < windowStart) continue;
            f.coldJson += r.json;
            f.coldJson.push_back('\n');
        }
    };
    gather(2, coldStatus_);
    if (mask & detail::v4TypeBit(3))
        for (auto& row : damageRowsAtCadence(windowStart, target)) {
            f.coldJson += row;
            f.coldJson.push_back('\n');
        }
    gather(4, coldLap_);
    if (!f.coldJson.empty()) f.coldJson.pop_back();   // no trailing newline
    return f;
}

} // namespace tnrp
