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
#include <queue>
#include <string_view>
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
                            { t, lf.last_lap_ms, (float)trackLengthM_ });
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
                    { t, lf.current_lap_ms, lf.lap_distance_m });
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
        if (detected == TnrdFormat::ChunkedV5) {
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
        for (const auto& l : indexedArchive_->laps()) {
            LapBlock block{}; block.lapNum=(int)l.lapNumber; block.startSessionTime=l.startSessionTime; block.endSessionTime=l.endSessionTime;
            lapBlocks_.emplace(block.lapNum, std::move(block));
            scannedLaps_.push_back({(int)l.lapNumber,l.startSessionTime,l.endSessionTime,(int)l.lapTimeMs});
            if (l.lapTimeMs && (!fastestLapMs_ || (int)l.lapTimeMs < fastestLapMs_)) { fastestLapMs_=(int)l.lapTimeMs; fastestLapNum_=(int)l.lapNumber; }
        }
        for (const auto& s : indexedArchive_->summary().lapStatus) {
            auto it=lapBlocks_.find((int)s.lapNumber);if(it!=lapBlocks_.end())it->second.slimStatus.push_back({"status",s.sessionTime,s.ersPct,s.tyreCompound,s.visualCompound});
        }
        strategyProtocol_ = static_cast<uint16_t>(outHeader.protocol >= 2024 ? outHeader.protocol : 2025);
        if (binaryPlayback_ && !buildIndexedSeekCache()) {
            std::fprintf(stderr, "[tnrd] load FAILED: indexed warm pass failed for '%s': %s\n",
                         path.c_str(), lastError_.c_str());
            close();
            return false;
        }
        if (indexedSeekCacheReady_) {
            // Pay the complete strategy replay cost under the existing
            // asynchronous loading overlay. Later random seeks restore the
            // nearest completed-lap checkpoint and process only its short tail.
            StrategyProcessor warmed(strategyProtocol_);
            (void)strategySnapshotAt(totalTime_, &warmed);
        }
        setCursor(startTime_);
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
    if (tempFile_) { std::fclose(tempFile_); tempFile_ = nullptr; }
    if (!tempPath_.empty()) {
        std::error_code ec;
        std::filesystem::remove(tempPath_, ec);
        tempPath_.clear();
    }
    loadedFormat_ = TnrdFormat::Unknown;
    indexedArchive_.reset();
    for (auto& lane : indexedPackedHistory_) lane = {};
    for (auto& rows : indexedSparseRows_) rows.clear();
    indexedSeekCacheReady_ = false;
    packedSeekCache_.clear();
    packedSeekLru_.clear();
    packedSeekCacheBytes_ = 0;
    v4PlaybackLanes_ = {};
    v4PlaybackLap_ = 0;
    v4PlaybackCursor_ = 0.0f;
    v4PlaybackPrepared_ = false;
    v4PlaybackPrefetchOutstanding_ = false;
    v4PlaybackDamageState_ = {};
    v4PlaybackDamageStateReady_ = false;
    index_.clear();
    lapBlocks_.clear();
    scannedLaps_.clear();
    scannedEvents_.clear();
    strategyCheckpoints_.clear();
    strategyProtocol_ = 2025;
    // Outstanding zero-copy seek buffers retain the old immutable store until
    // Electron has serialized them; the reader immediately releases its copy.
    hotBin_ = std::make_shared<std::vector<uint8_t>>();
    hotTimes_.clear();
    hotStart_.clear();
    hotCum_.clear();
    coldStatus_.clear();
    coldDamage_.clear();
    coldLap_.clear();
    legacyStrategyRows_.clear();
    scratch_.clear();
    scratch_.shrink_to_fit();
    fastestLapNum_ = 0;
    fastestLapMs_  = 0;
    initialFuelKg_ = -1.0;
    trackLengthM_ = 0;
    startTime_ = totalTime_ = 0.0f;
    tempFileSize_ = 0;
    playPos_ = 0;
    damageCadenceCursor_ = 0.0f;
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
        damageCadenceCursor_=t;return;
    }
    playPos_ = upperBoundTime(t);
    damageCadenceCursor_ = t;
}

std::vector<std::string> TnrdReader::damageRowsAtCadence(
    float fromTime, float toTime, bool includeFrom,
    const std::function<bool()>& cancelled) const {
    std::vector<std::string> out;
    if(cancelled&&cancelled())return out;
    std::vector<TimedRaw> v4Damage;
    const std::vector<TimedRaw>* source=&coldDamage_;
    if (isChunkedTnrd(loadedFormat_) && indexedArchive_ && !indexedSeekCacheReady_) {
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

bool TnrdReader::buildIndexedSeekCache() {
    if (!indexedArchive_ || !indexedArchive_->isOpen()) return false;

    for (auto& lane : indexedPackedHistory_) lane = {};
    for (auto& rows : indexedSparseRows_) rows.clear();
    coldStatus_.clear();
    coldDamage_.clear();
    coldLap_.clear();
    legacyStrategyRows_.clear();
    indexedSeekCacheReady_ = false;

    // Reserve from the directory before touching payloads. The packed record
    // byte size varies by hot family, but reserving the metadata exactly avoids
    // the largest source of allocation churn during a long recording load.
    std::array<size_t, 16> rowCounts{};
    for (const auto& chunk : indexedArchive_->chunks()) {
        const uint8_t type = static_cast<uint8_t>(chunk.rowType);
        if (type < rowCounts.size()) rowCounts[type] += chunk.rowCount;
    }
    for (const uint8_t type : {uint8_t{1}, uint8_t{11}, uint8_t{12}}) {
        indexedPackedHistory_[type].times.reserve(rowCounts[type]);
        indexedPackedHistory_[type].offsets.reserve(rowCounts[type] + 1);
    }
    for (uint8_t type = 5; type <= 10; ++type)
        indexedSparseRows_[type].reserve(rowCounts[type]);
    indexedSparseRows_[14].reserve(rowCounts[14]);
    coldStatus_.reserve(rowCounts[2]);
    coldDamage_.reserve(rowCounts[3]);
    coldLap_.reserve(rowCounts[4]);

    std::string callbackError;
    std::string archiveError;
    const bool walked = indexedArchive_->forEachChunk(
        0xFFFFFFFFu,
        [&](const detail::V4ChunkInfo& chunk, std::string_view plain) {
            const uint8_t type = static_cast<uint8_t>(chunk.rowType);
            size_t pos = 0;
            while (pos < plain.size()) {
                size_t nl = plain.find('\n', pos);
                if (nl == std::string_view::npos) nl = plain.size();
                if (nl > pos) {
                    const std::string_view line = plain.substr(pos, nl - pos);
                    const float time = scanSessionTime(line.data(), static_cast<int>(line.size()));
                    if (!std::isfinite(time) || time < 0.0f) {
                        callbackError = "indexed warm pass produced a row without a valid session_time";
                        return false;
                    }

                    if (type == 1 || type == 11 || type == 12) {
                        auto& lane = indexedPackedHistory_[type];
                        lane.offsets.push_back(lane.bytes.size());
                        lane.times.push_back(time);
                        if (!encodeV4HotRow(type, line, lane.bytes)) {
                            callbackError = "indexed warm pass could not encode a hot row";
                            return false;
                        }
                    } else if ((type >= 2 && type <= 10) || type == 14) {
                        std::string json(line);
                        if (type == 2) coldStatus_.push_back({time, json, chunk.sequence});
                        else if (type == 3) coldDamage_.push_back({time, json, chunk.sequence});
                        else if (type == 4) coldLap_.push_back({time, json, chunk.sequence});
                        if ((type >= 5 && type <= 10) || type == 14)
                            indexedSparseRows_[type].push_back({time, json, chunk.sequence});
                    }
                }
                if (nl == plain.size()) break;
                pos = nl + 1;
            }
            return true;
        }, &archiveError);
    if (!walked) {
        lastError_ = callbackError.empty()
            ? (archiveError.empty() ? "The indexed recording could not be warmed." : archiveError)
            : callbackError;
        return false;
    }

    auto sortTimed = [](std::vector<TimedRaw>& rows) {
        std::stable_sort(rows.begin(), rows.end(), [](const TimedRaw& a, const TimedRaw& b) {
            return a.t < b.t || (a.t == b.t && a.sequence < b.sequence);
        });
    };
    sortTimed(coldStatus_);
    sortTimed(coldDamage_);
    sortTimed(coldLap_);
    for (auto& rows : indexedSparseRows_) sortTimed(rows);

    // Writer ordering is normally monotonic within a row family. Preserve
    // correctness for recovered/early recordings that contain limited packet
    // reordering by sorting fixed packed records alongside their timestamps.
    for (const uint8_t type : {uint8_t{1}, uint8_t{11}, uint8_t{12}}) {
        auto& lane = indexedPackedHistory_[type];
        lane.offsets.push_back(lane.bytes.size());
        if (std::is_sorted(lane.times.begin(), lane.times.end())) continue;
        std::vector<size_t> order(lane.times.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            return lane.times[a] < lane.times[b];
        });
        std::vector<uint8_t> sortedBytes;
        std::vector<float> sortedTimes;
        std::vector<size_t> sortedOffsets;
        sortedBytes.reserve(lane.bytes.size());
        sortedTimes.reserve(lane.times.size());
        sortedOffsets.reserve(lane.offsets.size());
        for (size_t index : order) {
            sortedOffsets.push_back(sortedBytes.size());
            sortedTimes.push_back(lane.times[index]);
            sortedBytes.insert(sortedBytes.end(),
                lane.bytes.begin() + static_cast<std::ptrdiff_t>(lane.offsets[index]),
                lane.bytes.begin() + static_cast<std::ptrdiff_t>(lane.offsets[index + 1]));
        }
        sortedOffsets.push_back(sortedBytes.size());
        lane.bytes = std::move(sortedBytes);
        lane.times = std::move(sortedTimes);
        lane.offsets = std::move(sortedOffsets);
    }

    indexedSeekCacheReady_ = true;
    size_t packedBytes = 0;
    for (const auto& lane : indexedPackedHistory_) packedBytes += lane.bytes.size();
    size_t strategyRows = coldStatus_.size() + coldDamage_.size() + coldLap_.size();
    for (uint8_t type = 5; type <= 10; ++type)
        strategyRows += indexedSparseRows_[type].size();
    std::fprintf(stderr,
        "[tnrd] indexed warm cache: packed=%zu bytes status=%zu damage=%zu lap=%zu strategy=%zu\n",
        packedBytes, coldStatus_.size(), coldDamage_.size(), coldLap_.size(), strategyRows);
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
        std::vector<uint8_t> archiveTypes;
        for (const uint8_t type : types) {
            if (cancelled && cancelled()) return {};
            const std::vector<TimedRaw>* cached = nullptr;
            if (indexedSeekCacheReady_) {
                if (type == 2) cached = &coldStatus_;
                else if (type == 3) cached = &coldDamage_;
                else if (type == 4) cached = &coldLap_;
                else if ((type >= 5 && type <= 10) || type == 14)
                    cached = &indexedSparseRows_[type];
            }
            if (!cached) {
                archiveTypes.push_back(type);
                continue;
            }
            const auto it = std::upper_bound(cached->begin(), cached->end(), t,
                [](float value, const TimedRaw& row) { return value < row.t; });
            if (it != cached->begin()) out.emplace_back(type, std::prev(it)->json);
        }
        if (!archiveTypes.empty()) {
            std::vector<detail::V4TimedRow> rows;
            std::string error;
            if (!indexedArchive_->latestRows(t, archiveTypes, rows, &error, cancelled)) {
                if (!cancelled || !cancelled()) lastError_ = error;
                return {};
            }
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

StrategySnapshotRow TnrdReader::strategySnapshotAt(float t, StrategyProcessor* restoredProcessor,
                                                    const std::function<bool()>& cancelled) {
    if(cancelled&&cancelled())return StrategyProcessor(strategyProtocol_).snapshot();
    t = std::clamp(t, startTime_, totalTime_);
    StrategyProcessor processor(strategyProtocol_);
    float cursor = startTime_;
    bool restored = false;
    for (const auto& checkpoint : strategyCheckpoints_) {
        if (checkpoint.first > t) break;
        cursor = checkpoint.first;
        processor = checkpoint.second;
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
        (void)processor.snapshot();
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
    if (isChunkedTnrd(loadedFormat_) && indexedArchive_ && indexedSeekCacheReady_) {
        struct Cursor {
            const std::vector<TimedRaw>* rows{};
            size_t index{};
            size_t end{};
        };
        const auto later = [](const Cursor& a, const Cursor& b) {
            const auto& left = (*a.rows)[a.index];
            const auto& right = (*b.rows)[b.index];
            return left.t > right.t ||
                (left.t == right.t && left.sequence > right.sequence);
        };
        std::priority_queue<Cursor, std::vector<Cursor>, decltype(later)> pending(later);
        for (uint8_t type = 2; type <= 10; ++type) {
            const std::vector<TimedRaw>* rows = type == 2 ? &coldStatus_
                : type == 3 ? &coldDamage_
                : type == 4 ? &coldLap_
                : &indexedSparseRows_[type];
            const auto begin = includeFrom
                ? std::lower_bound(rows->begin(), rows->end(), fromTime,
                    [](const TimedRaw& row, float value) { return row.t < value; })
                : std::upper_bound(rows->begin(), rows->end(), fromTime,
                    [](float value, const TimedRaw& row) { return value < row.t; });
            const auto end = std::upper_bound(begin, rows->end(), toTime,
                [](float value, const TimedRaw& row) { return value < row.t; });
            if (begin != end)
                pending.push({rows, static_cast<size_t>(begin - rows->begin()),
                              static_cast<size_t>(end - rows->begin())});
        }
        while (!pending.empty()) {
            if (cancelled && cancelled()) return false;
            Cursor cursor = pending.top();
            pending.pop();
            const auto& row = (*cursor.rows)[cursor.index];
            callback(row.t, row.json);
            if (++cursor.index < cursor.end) pending.push(cursor);
        }
        return true;
    }
    if (isChunkedTnrd(loadedFormat_) && indexedArchive_ && !indexedSeekCacheReady_) {
        std::vector<detail::V4TimedRow> rows;
        std::string error;
        if (!indexedArchive_->rowsForRange(fromTime, toTime, kStrategyDependencyMask,
                                       rows, &error, cancelled)) {
            if(!cancelled||!cancelled())lastError_ = error;
            return false;
        }
        for (const auto& row : rows) {
            if(cancelled&&cancelled())return false;
            if (!includeFrom && row.sessionTime <= fromTime) continue;
            callback(row.sessionTime, row.json);
        }
        return true;
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
    if (playbackRowMask_ == mask) return;
    playbackRowMask_ = mask;
    if (isChunkedTnrd(loadedFormat_) && indexedArchive_) setCursor(cursorTime);
}

bool TnrdReader::currentLapAt(float t, float& startOut, int& numOut) const {
    if(isChunkedTnrd(loadedFormat_)&&indexedArchive_){numOut=indexedArchive_->lapAt(t);for(const auto&l:indexedArchive_->laps())if((int)l.lapNumber==numOut){startOut=l.startSessionTime;return true;}return false;}
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

std::string TnrdReader::lapBlocksMessage() const {
    PlaybackLapBlocksRow msg;
    for (const auto& kv : lapBlocks_) {
        const LapBlock& b = kv.second;
        // Slim vectors are empty unless binary playback built them; the copy is
        // what lets this method stay const and the message one-shot at load.
        msg.blocks.push_back({ b.lapNum, b.startSessionTime, b.endSessionTime,
                               b.slimTelemetry, b.slimStatus });
    }
    msg.fastestLapNum = fastestLapNum_;
    msg.initialFuelKg = initialFuelKg_;
    msg.tnrdVersion = loadedFormat_ == TnrdFormat::ChunkedV5 ? "TNRD_V5"
                    : loadedFormat_ == TnrdFormat::ChunkedV4 ? "TNRD_V4"
                    : loadedFormat_ == TnrdFormat::ZstdV3 ? "TNRD_V3"
                    : loadedFormat_ == TnrdFormat::ZstdV2 ? "TNRD_V2"
                    : loadedFormat_ == TnrdFormat::GzipV1 ? "TNRD_V1" : "";
    msg.deltaAvailable = loadedFormat_ == TnrdFormat::ZstdV3 || isChunkedTnrd(loadedFormat_);
    msg.lapDistanceAvailable = msg.deltaAvailable;
    msg.trackLengthM = msg.deltaAvailable ? trackLengthM_ : 0;
    msg.events.reserve(scannedEvents_.size());
    for (const auto& s : scannedEvents_) msg.events.push_back(glz::raw_json{ s });
    for (const auto& l : scannedLaps_)   msg.laps.push_back({ l.lapNum, l.lapTimeMs });
    return writeJson(msg);
}

std::string TnrdReader::getLapDataMessage(int lapNum, uint32_t rowTypeMask) const {
    auto it = lapBlocks_.find(lapNum);
    if (it == lapBlocks_.end()) return {};
    const LapBlock& b = it->second;
    PlaybackLapDataRow msg;
    msg.lapNum           = b.lapNum;
    msg.startSessionTime = b.startSessionTime;
    msg.endSessionTime   = b.endSessionTime;
    msg.rowTypeMask      = rowTypeMask;
    if(isChunkedTnrd(loadedFormat_)&&indexedArchive_){
        std::vector<detail::V4TimedRow> rows;std::string error;const uint32_t mask=rowTypeMask&(detail::v4TypeBit(1)|detail::v4TypeBit(2)|detail::v4TypeBit(3)|detail::v4TypeBit(4)|detail::v4TypeBit(11)|detail::v4TypeBit(12)|detail::v4TypeBit(13));
        if(!const_cast<detail::TnrdIndexedArchive*>(indexedArchive_.get())->rowsForLap((uint32_t)lapNum,mask,rows,&error))return {};
        for(const auto&r:rows){
            // A timed practice/quali lap number is reused across the in-lap,
            // garage and following out-lap. Those rows can share an indexed chunk
            // key with the next flying attempt, so honour the indexed flying
            // interval when materialising a lap.
            if(r.sessionTime<b.startSessionTime||r.sessionTime>b.endSessionTime)continue;
            if(r.rowType==1)msg.telemetry.push_back(glz::raw_json{r.json});
            else if(r.rowType==2)msg.statusHistory.push_back(glz::raw_json{r.json});
            else if(r.rowType==11)msg.motionHistory.push_back(glz::raw_json{r.json});
            else if(r.rowType==12)msg.motionExHistory.push_back(glz::raw_json{r.json});
            else if(r.rowType==4){LapScanFields lap{};(void)glz::read<kPartialRead>(lap,r.json);msg.lapProgress.push_back({r.sessionTime,lap.current_lap_ms,lap.lap_distance_m});}
            else if(r.rowType==13){PositionsRow pos{};(void)glz::read<kPartialRead>(pos,r.json);if(pos.player_idx>=0&&(size_t)pos.player_idx<pos.cars.size())msg.playerPositions.push_back({r.sessionTime,pos.cars[(size_t)pos.player_idx].x,pos.cars[(size_t)pos.player_idx].z});}
        }
        if(mask&detail::v4TypeBit(3))for(auto&row:damageRowsAtCadence(b.startSessionTime,b.endSessionTime))msg.damageHistory.push_back(glz::raw_json{row});
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

void TnrdReader::prepareV4PlaybackLap() {
    const float inf=std::numeric_limits<float>::infinity();
    for(auto& lane:v4PlaybackLanes_){lane.chunks.clear();lane.nextChunk=0;lane.nextPrefetched=false;lane.rows.clear();lane.rowPos=0;lane.maxDecodedTime=-inf;lane.safeThrough=inf;}
    if(!indexedArchive_||!indexedArchive_->isOpen()||v4PlaybackLap_<0){v4PlaybackPrepared_=true;return;}
    const auto& chunks=indexedArchive_->chunks();std::vector<size_t> selected;indexedArchive_->chunkIndicesForLap((uint32_t)v4PlaybackLap_,playbackRowMask_,selected);
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
        if(lane.nextChunk<lane.chunks.size())lane.safeThrough=-inf;
    }
    v4PlaybackPrepared_=true;
}

bool TnrdReader::loadV4PlaybackFrontier(float throughTime) {
    struct PendingChunk{size_t lane;size_t index;uint64_t sequence;};
    std::vector<PendingChunk> pending;const auto& chunks=indexedArchive_->chunks();float priority=std::numeric_limits<float>::infinity();
    for(const auto& lane:v4PlaybackLanes_)if(lane.nextChunk<lane.chunks.size()&&lane.safeThrough<=throughTime)priority=std::min(priority,lane.safeThrough);
    for(size_t i=0;i<v4PlaybackLanes_.size();++i){const auto& lane=v4PlaybackLanes_[i];if(lane.nextChunk<lane.chunks.size()&&lane.safeThrough==priority){const size_t index=lane.chunks[lane.nextChunk];pending.push_back({i,index,chunks[index].sequence});}}
    if(pending.empty())return false;
    std::sort(pending.begin(),pending.end(),[](const auto&a,const auto&b){return a.sequence<b.sequence;});
    std::vector<size_t> indices;indices.reserve(pending.size());for(const auto& item:pending)indices.push_back(item.index);
    std::vector<std::vector<detail::V4TimedRow>> decoded;std::string error;
    if(!indexedArchive_->rowsForChunks(indices,decoded,&error)){lastError_=error;return false;}
    for(const auto& item:pending)if(v4PlaybackLanes_[item.lane].nextPrefetched){indexedArchive_->cancelPrefetch();v4PlaybackPrefetchOutstanding_=false;break;}
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
            const bool inBlock=block==lapBlocks_.end()||(row.sessionTime>=block->second.startSessionTime&&row.sessionTime<=block->second.endSessionTime);
            if(!inBlock)continue;if(std::isfinite(row.sessionTime))lane.maxDecodedTime=std::max(lane.maxDecodedTime,row.sessionTime);
            if(pending[i].lane==3&&row.sessionTime<=v4PlaybackCursor_&&(!v4PlaybackDamageStateReady_||row.sessionTime>=v4PlaybackDamageState_.t)){v4PlaybackDamageState_={row.sessionTime,std::move(row.json)};v4PlaybackDamageStateReady_=true;}
            else if(row.sessionTime>v4PlaybackCursor_)lane.rows.push_back({row.sessionTime,row.sequence,std::move(row.json)});
        }
        std::inplace_merge(lane.rows.begin(),lane.rows.begin()+(ptrdiff_t)retained,lane.rows.end(),before);
        ++lane.nextChunk;lane.safeThrough=lane.nextChunk==lane.chunks.size()?inf:lane.maxDecodedTime-REORDER_WINDOW_S;
    }
    prefetchV4PlaybackChunk();
    return true;
}

void TnrdReader::prefetchV4PlaybackChunk() {
    if(v4PlaybackPrefetchOutstanding_)return;
    size_t best=v4PlaybackLanes_.size();const auto& chunks=indexedArchive_->chunks();
    for(size_t i=0;i<v4PlaybackLanes_.size();++i){const auto& lane=v4PlaybackLanes_[i];if(lane.nextPrefetched||lane.nextChunk>=lane.chunks.size())continue;if(best==v4PlaybackLanes_.size()||lane.safeThrough<v4PlaybackLanes_[best].safeThrough||(lane.safeThrough==v4PlaybackLanes_[best].safeThrough&&chunks[lane.chunks[lane.nextChunk]].sequence<chunks[v4PlaybackLanes_[best].chunks[v4PlaybackLanes_[best].nextChunk]].sequence))best=i;}
    if(best==v4PlaybackLanes_.size())return;auto& lane=v4PlaybackLanes_[best];lane.nextPrefetched=true;v4PlaybackPrefetchOutstanding_=true;indexedArchive_->prefetchChunk(lane.chunks[lane.nextChunk]);
}

bool TnrdReader::encodeV4HotRow(uint8_t type,std::string_view json,std::vector<uint8_t>& out){
    if(type==1){TelemetryRow row{};if(glz::read<kPartialRead>(row,json))return false;bin::encodeTelemetry(out,row);return true;}
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
        if(!v4PlaybackDamageStateReady_&&lastOfType&&!(*lastOfType)[3].empty()){const float seedTime=scanSessionTime((*lastOfType)[3].data(),(int)(*lastOfType)[3].size());if(seedTime<=damageCadenceCursor_){v4PlaybackDamageState_={seedTime,(*lastOfType)[3]};v4PlaybackDamageStateReady_=true;}}
        std::vector<TimedRaw> damageUpdates;auto rows=pullUntil(t);
        for(auto&row:rows){const uint8_t tid=scanType(row.data(),(int)row.size());if(!(playbackRowMask_&(1u<<tid)))continue;seenTypes|=(1u<<tid);if(tid==1||tid==11||tid==12){(void)encodeV4HotRow(tid,row,binOut);continue;}if(tid==3){if(lastOfType)(*lastOfType)[3]=row;damageUpdates.push_back({scanSessionTime(row.data(),(int)row.size()),std::move(row)});continue;}jsonOut+=row;jsonOut.push_back('\n');if(lastOfType&&tid<16)(*lastOfType)[tid]=row;}
        if((playbackRowMask_&(1u<<3))&&std::isfinite(damageCadenceCursor_)&&std::isfinite(cadenceEnd)&&cadenceEnd>=damageCadenceCursor_){
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
            if (lastOfType && tid < 16) (*lastOfType)[tid].assign(ld, (size_t)ll);
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

    if (isChunkedTnrd(loadedFormat_) && indexedArchive_ && indexedSeekCacheReady_) {
        auto binary = std::make_shared<std::vector<uint8_t>>();
        for (const uint8_t type : {uint8_t{1}, uint8_t{11}, uint8_t{12}}) {
            if (!(mask & detail::v4TypeBit(type))) continue;
            const auto& lane = indexedPackedHistory_[type];
            const size_t lo = static_cast<size_t>(std::lower_bound(
                lane.times.begin(), lane.times.end(), windowStart) - lane.times.begin());
            const size_t hi = static_cast<size_t>(std::upper_bound(
                lane.times.begin(), lane.times.end(), target) - lane.times.begin());
            if (hi <= lo || hi >= lane.offsets.size()) continue;
            binary->insert(binary->end(),
                lane.bytes.begin() + static_cast<std::ptrdiff_t>(lane.offsets[lo]),
                lane.bytes.begin() + static_cast<std::ptrdiff_t>(lane.offsets[hi]));
        }
        auto gather = [&](uint8_t type, const std::vector<TimedRaw>& rows) {
            if (!(mask & detail::v4TypeBit(type))) return;
            auto it = std::lower_bound(rows.begin(), rows.end(), windowStart,
                [](const TimedRaw& row, float value) { return row.t < value; });
            const auto end = std::upper_bound(it, rows.end(), target,
                [](float value, const TimedRaw& row) { return value < row.t; });
            for (; it != end; ++it) {
                f.coldJson += it->json;
                f.coldJson.push_back('\n');
            }
        };
        gather(2, coldStatus_);
        gather(4, coldLap_);
        if (mask & detail::v4TypeBit(3)) {
            auto damage = damageRowsAtCadence(windowStart, target, true, cancelled);
            if (cancelled && cancelled()) return {};
            if (!damage.empty()) {
                v4PlaybackDamageState_ = {
                    scanSessionTime(damage.back().data(), static_cast<int>(damage.back().size())),
                    damage.back()};
                v4PlaybackDamageStateReady_ = true;
            }
            for (auto& row : damage) {
                f.coldJson += row;
                f.coldJson.push_back('\n');
            }
        }
        if (!f.coldJson.empty()) f.coldJson.pop_back();
        if (!binary->empty()) {
            f.binaryStore = std::move(binary);
            f.binaryEnd = f.binaryStore->size();
        }
        return f;
    }

    if(isChunkedTnrd(loadedFormat_)&&indexedArchive_){
        std::vector<detail::V4TimedRow> rows;std::string error;
        const bool currentLapOnly=!allHistory&&windowSeconds<=0.0f;
        // Damage is reconstructed at its fixed cadence below. Excluding it from
        // this query avoids parsing and allocating the same JSON rows twice.
        const uint32_t directMask=mask&~detail::v4TypeBit(3);
        if(cancelled&&cancelled())return f;
        const bool loaded=!directMask||(currentLapOnly
            ? indexedArchive_->rowsForLapRange((uint32_t)indexedArchive_->lapAt(target),windowStart,target,directMask,rows,&error,cancelled)
            : indexedArchive_->rowsForRange(windowStart,target,directMask,rows,&error,cancelled));
        if(!loaded){if(!cancelled||!cancelled())lastError_=error;return f;}
        auto binary=std::make_shared<std::vector<uint8_t>>();for(const auto&r:rows){if(r.rowType==1||r.rowType==11||r.rowType==12)(void)encodeV4HotRowCached(r,*binary);else{f.coldJson+=r.json;f.coldJson.push_back('\n');}}
        if(cancelled&&cancelled())return {};
        if(mask&detail::v4TypeBit(3)){auto damage=damageRowsAtCadence(windowStart,target,true,cancelled);if(cancelled&&cancelled())return {};if(!damage.empty()){v4PlaybackDamageState_={scanSessionTime(damage.back().data(),(int)damage.back().size()),damage.back()};v4PlaybackDamageStateReady_=true;}for(auto&row:damage){f.coldJson+=row;f.coldJson.push_back('\n');}}if(!f.coldJson.empty())f.coldJson.pop_back();if(!binary->empty()){f.binaryStore=binary;f.binaryBegin=0;f.binaryEnd=binary->size();}return f;
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
