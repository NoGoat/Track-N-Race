#include "tnrp/TnrdReader.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <limits>
#include <iterator>
#include <string_view>
#include <unordered_set>

#ifndef _WIN32
#include <sys/types.h>
#endif

#include "tnrp/BinaryRows.h"
#include "TnrdCodec.h"
#include "TnrdV4.h"

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
    return tempFile_ != nullptr || (v4Archive_ && v4Archive_->isOpen());
}

bool TnrdReader::hasMore() const {
    if (loadedFormat_ != TnrdFormat::ChunkedV4) return playPos_ < index_.size();
    if (!v4Archive_ || !v4Archive_->isOpen()) return false;
    if(v4PlaybackPos_<v4PlaybackRows_.size())return true;
    const auto& laps=v4Archive_->laps();if(v4PlaybackLap_==0)return !laps.empty();const auto it=std::find_if(laps.begin(),laps.end(),[&](const auto&l){return (int)l.lapNumber==v4PlaybackLap_;});return it!=laps.end()&&std::next(it)!=laps.end();
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
                    if ((loadedFormat_ == TnrdFormat::ZstdV3 || loadedFormat_ == TnrdFormat::ChunkedV4) &&
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
                     (loadedFormat_ == TnrdFormat::ZstdV3 || loadedFormat_ == TnrdFormat::ChunkedV4) &&
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
    if (isLegacyZstdStream(detected))
        std::fprintf(stderr,"[tnrd] load: '%s' (Zstandard stream; TNRD version is in the header)\n",path.c_str());
    else
        std::fprintf(stderr, "[tnrd] load: '%s' (%s)\n", path.c_str(), toString(detected));
    if (detected == TnrdFormat::ChunkedV4) {
        std::string error;
        v4Archive_ = std::make_unique<detail::TnrdV4Archive>();
        if (!v4Archive_->open(path, outHeader, &error)) {
            lastError_ = error.empty() ? "The V4 recording could not be read." : error;
            close();
            return false;
        }
        loadedFormat_ = TnrdFormat::ChunkedV4;
        trackLengthM_ = outHeader.track_length_m.value_or(0);
        startTime_ = v4Archive_->startTime();
        totalTime_ = v4Archive_->totalTime();
        initialFuelKg_ = v4Archive_->summary().initialFuelKg;
        scannedEvents_.clear();
        scannedEvents_.reserve(v4Archive_->summary().events.size());
        for (std::string event : v4Archive_->summary().events) {
            while (!event.empty() && (event.back() == '\n' || event.back() == '\r'))
                event.pop_back();
            glz::generic parsedEvent;
            if (event.empty() || glz::read_json(parsedEvent, event)) {
                lastError_ = "The V4 recording contains an invalid race-event summary.";
                close();
                return false;
            }
            scannedEvents_.push_back(std::move(event));
        }
        for (const auto& l : v4Archive_->laps()) {
            LapBlock block{}; block.lapNum=(int)l.lapNumber; block.startSessionTime=l.startSessionTime; block.endSessionTime=l.endSessionTime;
            lapBlocks_.emplace(block.lapNum, std::move(block));
            scannedLaps_.push_back({(int)l.lapNumber,l.startSessionTime,l.endSessionTime,(int)l.lapTimeMs});
            if (l.lapTimeMs && (!fastestLapMs_ || (int)l.lapTimeMs < fastestLapMs_)) { fastestLapMs_=(int)l.lapTimeMs; fastestLapNum_=(int)l.lapNumber; }
        }
        for (const auto& s : v4Archive_->summary().lapStatus) {
            auto it=lapBlocks_.find((int)s.lapNumber);if(it!=lapBlocks_.end())it->second.slimStatus.push_back({"status",s.sessionTime,s.ersPct,s.tyreCompound,s.visualCompound});
        }
        setCursor(startTime_);
        std::fprintf(stderr,"[tnrd] load OK: format=%s chunks=%zu laps=%zu start=%.2f total=%.2f track='%s' session='%s'\n",toString(loadedFormat_),v4Archive_->chunks().size(),v4Archive_->laps().size(),startTime_,totalTime_,outHeader.track_name.c_str(),outHeader.session_name.c_str());
        return true;
    }
    namespace fs = std::filesystem;
    // Prefix shared with the Electron app ("tracknrace_temp_") so either app's
    // startup sweep reclaims the other's leftovers (see sweepStaleTempFiles).
    auto tmpName = "tracknrace_temp_" + std::to_string(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count()) + ".tmp";
    tempPath_ = (fs::temp_directory_path() / tmpName).string();

    bool partial = false;
    std::string decompressError;
    if (!detail::decompressTnrd(path, tempPath_, format, &partial, &decompressError)) {
        lastError_ = decompressError.empty() ? "The recording could not be decompressed." : decompressError;
        std::fprintf(stderr, "[tnrd] load FAILED: decompress step for '%s': %s\n",
                     path.c_str(), lastError_.c_str());
        close();
        return false;
    }
    if (partial)
        std::fprintf(stderr, "[tnrd] load: recovered a truncated %s stream; final partial row will be dropped\n",
                     toString(format));

    {
        std::FILE* f = detail::openTnrdFile(tempPath_, "rb");
        if (!f) {
            lastError_ = "The decompressed temporary file could not be opened.";
            std::fprintf(stderr, "[tnrd] load FAILED: cannot reopen decompressed temp '%s'\n",
                         tempPath_.c_str());
            close();
            return false;
        }
        std::string line;
        char hb[8192];
        while (std::fgets(hb, sizeof(hb), f)) {
            line += hb;
            if (!line.empty() && line.back() == '\n') break;
            if (std::strlen(hb) < sizeof(hb) - 1) break;
        }
        std::fclose(f);
        outHeader = HeaderRow{};
        auto ec = glz::read<kPartialRead>(outHeader, line);
        if (ec) {
            lastError_ = "The recording header is missing or contains invalid JSON.";
            std::fprintf(stderr, "[tnrd] load FAILED: header JSON parse error (first line, %zu bytes)\n",
                         line.size());
            close();
            return false;
        }
        TnrdFormat headerFormat = TnrdFormat::Unknown;
        if (outHeader.magic == "TNRD_V1") headerFormat = TnrdFormat::GzipV1;
        else if (outHeader.magic == "TNRD_V2") headerFormat = TnrdFormat::ZstdV2;
        else if (outHeader.magic == "TNRD_V3") headerFormat = TnrdFormat::ZstdV3;
        const bool compressionOk = isZstd(detected)
            ? (isZstd(headerFormat) && outHeader.compression && *outHeader.compression == "zstd")
            : (headerFormat == TnrdFormat::GzipV1 &&
               (!outHeader.compression || *outHeader.compression == "gzip"));
        if (!compressionOk) {
            lastError_ = "The recording header does not match its compression format.";
            std::fprintf(stderr,
                         "[tnrd] load FAILED: header/container mismatch: container=%s magic='%s' compression='%s'\n",
                         toString(format), outHeader.magic.c_str(),
                         outHeader.compression ? outHeader.compression->c_str() : "");
            close();
            return false;
        }
        loadedFormat_ = headerFormat;
        trackLengthM_ = outHeader.track_length_m.value_or(0);
    }

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
    v4Archive_.reset();
    v4PlaybackRows_.clear();
    v4PlaybackRows_.shrink_to_fit();
    v4PlaybackPos_ = 0;
    v4PlaybackLap_ = 0;
    index_.clear();
    lapBlocks_.clear();
    scannedLaps_.clear();
    scannedEvents_.clear();
    // Outstanding zero-copy seek buffers retain the old immutable store until
    // Electron has serialized them; the reader immediately releases its copy.
    hotBin_ = std::make_shared<std::vector<uint8_t>>();
    hotTimes_.clear();
    hotStart_.clear();
    hotCum_.clear();
    coldStatus_.clear();
    coldDamage_.clear();
    coldLap_.clear();
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
    if (loadedFormat_ == TnrdFormat::ChunkedV4) return {};
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
    if (loadedFormat_ == TnrdFormat::ChunkedV4) {
        v4PlaybackLap_ = v4Archive_ ? v4Archive_->lapAt(t) : 0;
        (void)loadV4PlaybackLap(v4PlaybackLap_);
        v4PlaybackPos_ = (size_t)(std::upper_bound(v4PlaybackRows_.begin(),v4PlaybackRows_.end(),t,[](float value,const TimedRaw&r){return value<r.t;})-v4PlaybackRows_.begin());
        damageCadenceCursor_=t;return;
    }
    playPos_ = upperBoundTime(t);
    damageCadenceCursor_ = t;
}

std::vector<std::string> TnrdReader::damageRowsAtCadence(
    float fromTime, float toTime, bool includeFrom) const {
    std::vector<std::string> out;
    std::vector<TimedRaw> v4Damage;
    const std::vector<TimedRaw>* source=&coldDamage_;
    if (loadedFormat_==TnrdFormat::ChunkedV4&&v4Archive_){std::vector<detail::V4TimedRow> rows,initial;std::string error;auto* archive=const_cast<detail::TnrdV4Archive*>(v4Archive_.get());if(!archive->latestRows(fromTime,{3},initial,&error)||!archive->rowsForRange(fromTime,toTime,detail::v4TypeBit(3),rows,&error))return out;if(!initial.empty())v4Damage.push_back({initial.back().sessionTime,std::move(initial.back().json)});for(auto&r:rows)if(v4Damage.empty()||r.sessionTime!=v4Damage.back().t||r.json!=v4Damage.back().json)v4Damage.push_back({r.sessionTime,std::move(r.json)});source=&v4Damage;}
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

// Latest row of each requested type at or before t. Walks the index backward and
// reads only the matched lines (never the whole window), returning them ordered by
// file position. Stops as soon as every requested type has been found.
std::vector<std::pair<uint8_t, std::string>> TnrdReader::latestOfTypesTagged(
    float t, const std::vector<uint8_t>& types) {
    std::vector<std::pair<uint8_t, std::string>> out;
    if(loadedFormat_==TnrdFormat::ChunkedV4&&v4Archive_){std::vector<detail::V4TimedRow> rows;std::string error;if(!v4Archive_->latestRows(t,types,rows,&error)){lastError_=error;return out;}for(auto&r:rows)out.emplace_back(r.rowType,std::move(r.json));return out;}
    if (index_.empty() || types.empty()) return out;
    size_t pos = upperBoundTime(t);
    std::unordered_set<uint8_t> wanted(types.begin(), types.end());
    std::vector<size_t> snapshot;
    for (size_t i = pos; i-- > 0 && !wanted.empty(); )
        if (wanted.erase(index_[i].type)) snapshot.push_back(i);
    std::sort(snapshot.begin(), snapshot.end());
    for (size_t idx : snapshot) {
        std::string s = readLine(index_[idx].offset);
        if (!s.empty()) out.emplace_back(index_[idx].type, std::move(s));
    }
    return out;
}

std::vector<std::string> TnrdReader::latestOfTypes(float t, const std::vector<uint8_t>& types) {
    std::vector<std::string> out;
    for (auto& [tid, line] : latestOfTypesTagged(t, types)) {
        (void)tid;
        out.push_back(std::move(line));
    }
    return out;
}

std::vector<std::string> TnrdReader::stateSnapshot(float t) {
    return latestOfTypes(t, { std::begin(STATE_TYPE_IDS), std::end(STATE_TYPE_IDS) });
}

std::vector<std::string> TnrdReader::readRange(float fromTime, float toTime) {
    std::vector<std::string> out;
    if(loadedFormat_==TnrdFormat::ChunkedV4&&v4Archive_){std::vector<detail::V4TimedRow> rows;std::string error;if(!v4Archive_->rowsForRange(fromTime,toTime,0xFFFFFFFFu,rows,&error)){lastError_=error;return out;}for(auto&r:rows)out.push_back(std::move(r.json));return out;}
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

void TnrdReader::setPlaybackRowMask(uint32_t mask, float cursorTime) {
    if (playbackRowMask_ == mask) return;
    playbackRowMask_ = mask;
    if (loadedFormat_ == TnrdFormat::ChunkedV4 && v4Archive_)
        setCursor(cursorTime);
}

bool TnrdReader::currentLapAt(float t, float& startOut, int& numOut) const {
    if(loadedFormat_==TnrdFormat::ChunkedV4&&v4Archive_){numOut=v4Archive_->lapAt(t);for(const auto&l:v4Archive_->laps())if((int)l.lapNumber==numOut){startOut=l.startSessionTime;return true;}return false;}
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
    msg.tnrdVersion = loadedFormat_ == TnrdFormat::ChunkedV4 ? "TNRD_V4"
                    : loadedFormat_ == TnrdFormat::ZstdV3 ? "TNRD_V3"
                    : loadedFormat_ == TnrdFormat::ZstdV2 ? "TNRD_V2"
                    : loadedFormat_ == TnrdFormat::GzipV1 ? "TNRD_V1" : "";
    msg.deltaAvailable = loadedFormat_ == TnrdFormat::ZstdV3 || loadedFormat_ == TnrdFormat::ChunkedV4;
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
    if(loadedFormat_==TnrdFormat::ChunkedV4&&v4Archive_){
        std::vector<detail::V4TimedRow> rows;std::string error;const uint32_t mask=rowTypeMask&(detail::v4TypeBit(1)|detail::v4TypeBit(2)|detail::v4TypeBit(3)|detail::v4TypeBit(4)|detail::v4TypeBit(11)|detail::v4TypeBit(12)|detail::v4TypeBit(13));
        if(!const_cast<detail::TnrdV4Archive*>(v4Archive_.get())->rowsForLap((uint32_t)lapNum,mask,rows,&error))return {};
        for(const auto&r:rows){
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
    if ((rowTypeMask&detail::v4TypeBit(4)) && (loadedFormat_ == TnrdFormat::ZstdV3 || loadedFormat_ == TnrdFormat::ChunkedV4))
        msg.lapProgress = b.lapProgress;
    if(rowTypeMask&detail::v4TypeBit(13))msg.playerPositions = b.playerPositions;
    return writeJson(msg);
}

bool TnrdReader::loadV4PlaybackLap(int lapNum) {
    v4PlaybackRows_.clear();v4PlaybackPos_=0;v4PlaybackLap_=lapNum;
    if(!v4Archive_||!v4Archive_->isOpen()||lapNum<0)return false;
    std::vector<detail::V4TimedRow> rows;std::string error;
    if(!v4Archive_->rowsForLap((uint32_t)lapNum,playbackRowMask_,rows,&error)){lastError_=error;return false;}
    v4PlaybackRows_.reserve(rows.size());for(auto&r:rows)v4PlaybackRows_.push_back({r.sessionTime,std::move(r.json)});return true;
}

bool TnrdReader::encodeV4HotRow(uint8_t type,std::string_view json,std::vector<uint8_t>& out){
    if(type==1){TelemetryRow row{};if(glz::read<kPartialRead>(row,json))return false;bin::encodeTelemetry(out,row);return true;}
    if(type==11){MotionRow row{};if(glz::read<kPartialRead>(row,json))return false;bin::encodeMotion(out,row);return true;}
    if(type==12){MotionExRow row{};if(glz::read<kPartialRead>(row,json))return false;bin::encodeMotionEx(out,row);return true;}
    return false;
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
    if(loadedFormat_==TnrdFormat::ChunkedV4&&v4Archive_){
        for(;;){while(v4PlaybackPos_<v4PlaybackRows_.size()&&v4PlaybackRows_[v4PlaybackPos_].t<=t)out.push_back(v4PlaybackRows_[v4PlaybackPos_++].json);if(v4PlaybackPos_<v4PlaybackRows_.size())break;const auto&laps=v4Archive_->laps();auto it=v4PlaybackLap_==0?laps.begin():std::find_if(laps.begin(),laps.end(),[&](const auto&l){return (int)l.lapNumber==v4PlaybackLap_;});if(v4PlaybackLap_!=0&&it!=laps.end())++it;if(it==laps.end()||it->startSessionTime>t)break;if(!loadV4PlaybackLap((int)it->lapNumber))break;}return out;
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
    if(loadedFormat_==TnrdFormat::ChunkedV4&&v4Archive_){
        auto rows=pullUntil(t);for(auto&row:rows){const uint8_t tid=scanType(row.data(),(int)row.size());if(!(playbackRowMask_&(1u<<tid)))continue;seenTypes|=(1u<<tid);if(tid==1||tid==11||tid==12){(void)encodeV4HotRow(tid,row,binOut);continue;}if(tid==3){if(lastOfType)(*lastOfType)[3]=row;continue;}jsonOut+=row;jsonOut.push_back('\n');if(lastOfType&&tid<16)(*lastOfType)[tid]=row;}
        if(playbackRowMask_&(1u<<3))for(auto&row:damageRowsAtCadence(damageCadenceCursor_,cadenceEnd,false)){jsonOut+=row;jsonOut.push_back('\n');seenTypes|=(1u<<3);if(lastOfType)(*lastOfType)[3]=row;}damageCadenceCursor_=cadenceEnd;return;
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
                                            float windowSeconds, bool includeMandatoryState) {
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

    if(loadedFormat_==TnrdFormat::ChunkedV4&&v4Archive_){
        std::vector<detail::V4TimedRow> rows;std::string error;
        if(!v4Archive_->rowsForRange(windowStart,target,mask,rows,&error)){lastError_=error;return f;}
        auto binary=std::make_shared<std::vector<uint8_t>>();for(const auto&r:rows){if(r.rowType==1||r.rowType==11||r.rowType==12)(void)encodeV4HotRow(r.rowType,r.json,*binary);else if(r.rowType!=3){f.coldJson+=r.json;f.coldJson.push_back('\n');}}
        if(mask&detail::v4TypeBit(3))for(auto&row:damageRowsAtCadence(windowStart,target)){f.coldJson+=row;f.coldJson.push_back('\n');}if(!f.coldJson.empty())f.coldJson.pop_back();if(!binary->empty()){f.binaryStore=binary;f.binaryBegin=0;f.binaryEnd=binary->size();}return f;
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
