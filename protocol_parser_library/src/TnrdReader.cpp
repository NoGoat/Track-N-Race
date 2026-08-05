#include "tnrp/TnrdReader.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <string_view>
#include <unordered_set>

#include "tnrp/BinaryRows.h"
#include "TnrdCodec.h"

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

TnrdReader::~TnrdReader() { close(); }

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

void TnrdReader::buildIndex(const std::string& filePath) {
    index_.clear();
    lapBlocks_.clear();
    scannedLaps_.clear();
    scannedEvents_.clear();
    hotBin_.clear();
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

    std::FILE* f = std::fopen(filePath.c_str(), "rb");
    if (!f) {
        std::fprintf(stderr, "[tnrd] buildIndex: cannot open '%s'\n", filePath.c_str());
        return;
    }
    // Reserve against vector regrowth during the scan: rows average well under
    // ~96 bytes of JSONL, so this slightly over-reserves and settles in one go.
    {
        std::fseek(f, 0, SEEK_END);
        long sz = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (sz > 0) index_.reserve((size_t)sz / 96);
    }
    { int c; while ((c = std::fgetc(f)) != EOF && c != '\n') {} }  // skip header
    if (binaryPlayback_) hotCum_.push_back(0);

    constexpr long CHUNK = 2 * 1024 * 1024;
    std::vector<char> buf(CHUNK);
    std::string partial;
    long partialOffset = std::ftell(f);
    float lastT = 0.0f;
    bool  haveStart = false;

    int   curLapNum   = -1;
    float curLapStart = 0.0f;
    int   latestHistoryLapNum = 0;
    int   latestHistoryLapTimeMs = 0;

    auto commitLine = [&](const char* ld, int ll, long lineOffset) {
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
                hotStart_.push_back(hotBin_.size());
                hotTimes_.push_back(t);
                bin::encodeTelemetry(hotBin_, telRow);
            } else if (tid == 11) {
                MotionRow r;
                (void)glz::read<kPartialRead>(r, sv);
                hotStart_.push_back(hotBin_.size());
                hotTimes_.push_back(t);
                bin::encodeMotion(hotBin_, r);
            } else if (tid == 12) {
                MotionExRow r;
                (void)glz::read<kPartialRead>(r, sv);
                hotStart_.push_back(hotBin_.size());
                hotTimes_.push_back(t);
                bin::encodeMotionEx(hotBin_, r);
            } else if (tid == 2) {
                if (initialFuelKg_ < 0.0) initialFuelKg_ = statusRow.fuel_kg;
                coldStatus_.push_back({ t, std::string(ld, ll) });
            } else if (tid == 4) {
                coldLap_.push_back({ t, std::string(ld, ll) });
            }
            hotCum_.push_back((uint32_t)hotTimes_.size());
        }

        if (tid != 1 && tid != 2 && tid != 4 && tid != 6 && tid != 3 && tid != 11 && tid != 12 && tid != 14) return;

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
                    if (loadedFormat_ == TnrdFormat::ZstdV3 && trackLengthM_ > 0 &&
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
            else if (tid == 4 && loadedFormat_ == TnrdFormat::ZstdV3 &&
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

    for (;;) {
        long chunkStart = std::ftell(f);
        size_t n = std::fread(buf.data(), 1, (size_t)CHUNK, f);
        if (n == 0) break;
        const char* base = buf.data();
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
    }
    // A leftover partial here means the file ended without a trailing newline.
    // The writer always terminates every row (and the header) with '\n', so a
    // cleanly closed stream leaves nothing pending — a non-empty partial marks a
    // truncated tail (crash mid-write). Drop it rather than index a corrupt row.
    std::fclose(f);

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
    if (binaryPlayback_) hotStart_.push_back(hotBin_.size());   // sentinel end offset
    damageCadenceCursor_ = startTime_;
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
    const bool codecMatches = detected == format || (isZstd(detected) && isZstd(format));
    if (!codecMatches) {
        lastError_ = detected == TnrdFormat::Unknown
            ? (detectError.empty() ? "The file format could not be identified." : detectError)
            : std::string("Expected ") + toString(format) + " but found " + toString(detected) + ".";
        std::fprintf(stderr, "[tnrd] load FAILED: requested %s but file is %s ('%s')\n",
                     toString(format), toString(detected), path.c_str());
        return false;
    }
    std::fprintf(stderr, "[tnrd] load: '%s' (%s)\n", path.c_str(), toString(format));
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
        std::FILE* f = std::fopen(tempPath_.c_str(), "rb");
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

    buildIndex(tempPath_);
    if (index_.empty()) {
        lastError_ = "The recording contains no readable telemetry rows.";
        std::fprintf(stderr, "[tnrd] load FAILED: index empty (no indexable rows) for '%s'\n", path.c_str());
        close();
        return false;
    }

    tempFile_ = std::fopen(tempPath_.c_str(), "rb");
    if (!tempFile_) {
        lastError_ = "The decompressed recording could not be opened for playback.";
        std::fprintf(stderr, "[tnrd] load FAILED: final reopen of temp '%s' failed\n", tempPath_.c_str());
        close();
        return false;
    }
    std::fseek(tempFile_, 0, SEEK_END);
    tempFileSize_ = std::ftell(tempFile_);
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
    index_.clear();
    lapBlocks_.clear();
    scannedLaps_.clear();
    scannedEvents_.clear();
    hotBin_.clear();
    hotBin_.shrink_to_fit();     // the hot store can be tens of MB — release it
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
std::string TnrdReader::readLine(long offset) {
    if (!tempFile_) return {};
    if (std::fseek(tempFile_, offset, SEEK_SET) != 0) return {};
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
    playPos_ = upperBoundTime(t);
    damageCadenceCursor_ = t;
}

std::vector<std::string> TnrdReader::damageRowsAtCadence(
    float fromTime, float toTime, bool includeFrom) const {
    std::vector<std::string> out;
    if (coldDamage_.empty() || !std::isfinite(fromTime) ||
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
        coldDamage_.begin(), coldDamage_.end(), (float)(firstTick / RATE),
        [](float t, const TimedRaw& row) { return t < row.t; });
    size_t stateIndex = nextState == coldDamage_.begin()
        ? coldDamage_.size()
        : (size_t)(nextState - coldDamage_.begin() - 1);

    out.reserve((size_t)(lastTick - firstTick + 1));
    for (long long tick = firstTick; tick <= lastTick; ++tick) {
        const float sampleTime = (float)((double)tick / RATE);
        while (stateIndex != coldDamage_.size() &&
               stateIndex + 1 < coldDamage_.size() &&
               coldDamage_[stateIndex + 1].t <= sampleTime + (float)EPS) {
            ++stateIndex;
        }
        if (stateIndex == coldDamage_.size()) {
            if (coldDamage_.front().t > sampleTime + (float)EPS) continue;
            stateIndex = 0;
        }
        std::string row = coldDamage_[stateIndex].json;
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
    if (!tempFile_ || index_.empty()) return out;
    size_t lo = lowerBoundTime(fromTime);
    size_t hi = upperBoundTime(toTime);
    if (lo >= hi) return out;

    long startOff = index_[lo].offset;
    long endOff   = (hi < index_.size()) ? index_[hi].offset : tempFileSize_;
    long len = endOff - startOff;
    if (len <= 0) return out;

    std::vector<char> buf((size_t)len);
    if (std::fseek(tempFile_, startOff, SEEK_SET) != 0) return out;
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

bool TnrdReader::currentLapAt(float t, float& startOut, int& numOut) const {
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
    msg.tnrdVersion = loadedFormat_ == TnrdFormat::ZstdV3 ? "TNRD_V3"
                    : loadedFormat_ == TnrdFormat::ZstdV2 ? "TNRD_V2"
                    : loadedFormat_ == TnrdFormat::GzipV1 ? "TNRD_V1" : "";
    msg.deltaAvailable = loadedFormat_ == TnrdFormat::ZstdV3;
    msg.trackLengthM = loadedFormat_ == TnrdFormat::ZstdV3 ? trackLengthM_ : 0;
    msg.events.reserve(scannedEvents_.size());
    for (const auto& s : scannedEvents_) msg.events.push_back(glz::raw_json{ s });
    for (const auto& l : scannedLaps_)   msg.laps.push_back({ l.lapNum, l.lapTimeMs });
    return writeJson(msg);
}

std::string TnrdReader::getLapDataMessage(int lapNum) const {
    auto it = lapBlocks_.find(lapNum);
    if (it == lapBlocks_.end()) return {};
    const LapBlock& b = it->second;
    PlaybackLapDataRow msg;
    msg.lapNum           = b.lapNum;
    msg.startSessionTime = b.startSessionTime;
    msg.endSessionTime   = b.endSessionTime;
    auto fill = [](std::vector<glz::raw_json>& dst, const std::vector<TimedRaw>& src) {
        dst.reserve(src.size());
        for (const auto& e : src) dst.push_back(glz::raw_json{ e.json });
    };
    fill(msg.telemetry,       b.telemetry);
    fill(msg.statusHistory,   b.statusHistory);
    fill(msg.motionHistory,   b.motionHistory);
    fill(msg.motionExHistory, b.motionExHistory);
    const auto damageRows = damageRowsAtCadence(
        b.startSessionTime, b.endSessionTime);
    for (const auto& row : damageRows) {
        msg.damageHistory.push_back(glz::raw_json{ row });
    }
    if (loadedFormat_ == TnrdFormat::ZstdV3)
        msg.lapProgress = b.lapProgress;
    return writeJson(msg);
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
    if (!tempFile_ || fromIdx >= toIdx || toIdx > index_.size()) return 0;
    long startOff = index_[fromIdx].offset;
    long endOff   = (toIdx < index_.size()) ? index_[toIdx].offset : tempFileSize_;
    long len = endOff - startOff;
    if (len <= 0) return 0;
    if (scratch_.size() < (size_t)len) scratch_.resize((size_t)len);
    if (std::fseek(tempFile_, startOff, SEEK_SET) != 0) return 0;
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
    size_t end = pullEnd(t);

    // Hot rows: the range's records are contiguous in the packed store, so the
    // whole tick's hot payload is a single byte-slice append.
    if (end > playPos_ && !hotCum_.empty()) {
        size_t hotLo = hotCum_[playPos_], hotHi = hotCum_[end];
        if (hotHi > hotLo)
            binOut.insert(binOut.end(),
                          hotBin_.begin() + (long)hotStart_[hotLo],
                          hotBin_.begin() + (long)hotStart_[hotHi]);
    }

    // Cold rows: contiguous read, hot lines skipped (they went out as binary).
    size_t got = end > playPos_ ? readBlock(playPos_, end) : 0;
    if (got > 0) {
        walkBlockLines(scratch_.data(), got, playPos_, end - playPos_,
                       [&](size_t idx, const char* ld, int ll) {
            uint8_t tid = index_[idx].type;
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

    auto damageRows = damageRowsAtCadence(
        damageCadenceCursor_, cadenceEnd, false);
    for (auto& row : damageRows) {
        jsonOut += row;
        jsonOut.push_back('\n');
        seenTypes |= (1u << 3);
        if (lastOfType) (*lastOfType)[3] = row;
    }
    damageCadenceCursor_ = cadenceEnd;
}

TnrdReader::SeekFlush TnrdReader::seekFlush(float target, float currentLapStart) {
    SeekFlush f;
    // Backfill window: the whole current lap, but at most 10 minutes — mirrors
    // the TS engine's extractAndBroadcastSeek.
    float windowStart = std::max(target - 600.0f, currentLapStart);

    if (!hotTimes_.empty()) {
        size_t lo = std::lower_bound(hotTimes_.begin(), hotTimes_.end(), windowStart) - hotTimes_.begin();
        size_t hi = std::upper_bound(hotTimes_.begin(), hotTimes_.end(), target) - hotTimes_.begin();
        if (hi > lo)
            f.binary.assign(hotBin_.begin() + (long)hotStart_[lo],
                            hotBin_.begin() + (long)hotStart_[hi]);
    }

    // Cold rows: full linear scan (small, ~2 Hz), robust to the occasional
    // out-of-order row — exactly like the TS gatherCold.
    auto gather = [&](const std::vector<TimedRaw>& rows) {
        for (const auto& r : rows) {
            if (r.t > target || r.t < windowStart) continue;
            f.coldJson += r.json;
            f.coldJson.push_back('\n');
        }
    };
    gather(coldStatus_);
    for (auto& row : damageRowsAtCadence(windowStart, target)) {
        f.coldJson += row;
        f.coldJson.push_back('\n');
    }
    gather(coldLap_);
    if (!f.coldJson.empty()) f.coldJson.pop_back();   // no trailing newline
    return f;
}

} // namespace tnrp
