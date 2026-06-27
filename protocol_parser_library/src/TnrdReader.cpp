#include "tnrp/TnrdReader.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <unordered_set>

#include <zlib.h>

#ifdef _WIN32
#  include <windows.h>
#endif

namespace tnrp {

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
            return 0;
        }
    }
    return 0;
}

static gzFile gzOpenRead(const std::string& utf8Path) {
#ifdef _WIN32
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8Path.c_str(), -1, nullptr, 0);
    if (wlen <= 0) return nullptr;
    std::wstring wpath(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8Path.c_str(), -1, wpath.data(), wlen);
    return gzopen_w(wpath.c_str(), "rb");
#else
    return gzopen(utf8Path.c_str(), "rb");
#endif
}

TnrdReader::~TnrdReader() { close(); }

bool TnrdReader::decompress(const std::string& srcPath, const std::string& destPath) {
    gzFile gz = gzOpenRead(srcPath);
    if (!gz) return false;
    std::FILE* out = std::fopen(destPath.c_str(), "wb");
    if (!out) { gzclose(gz); return false; }
    char buf[131072];
    int n;
    bool ok = true;
    while ((n = gzread(gz, buf, sizeof(buf))) > 0)
        if (std::fwrite(buf, 1, (size_t)n, out) != (size_t)n) { ok = false; break; }
    if (n < 0) ok = false;
    gzclose(gz);
    std::fclose(out);
    return ok;
}

void TnrdReader::buildIndex(const std::string& filePath) {
    index_.clear();
    lapBlocks_.clear();
    scannedLaps_.clear();
    scannedEvents_.clear();
    fastestLapNum_ = 0;
    fastestLapMs_  = 0;
    startTime_ = 0.0f;
    totalTime_ = 0.0f;
    playPos_   = 0;

    std::FILE* f = std::fopen(filePath.c_str(), "rb");
    if (!f) return;
    { int c; while ((c = std::fgetc(f)) != EOF && c != '\n') {} }  // skip header

    constexpr long CHUNK = 2 * 1024 * 1024;
    std::vector<char> buf(CHUNK);
    std::string partial;
    long partialOffset = std::ftell(f);
    float lastT = 0.0f;
    bool  haveStart = false;

    int   curLapNum   = -1;
    float curLapStart = 0.0f;

    auto commitLine = [&](const char* ld, int ll, long lineOffset) {
        if (ll <= 1) return;
        float t = scanSessionTime(ld, ll);
        if (t < 0.0f) t = lastT;
        else          lastT = t;
        if (!haveStart) { startTime_ = t; haveStart = true; }
        uint8_t tid = scanType(ld, ll);
        index_.push_back({ lineOffset, t, tid });
        totalTime_ = std::max(totalTime_, t);

        if (tid != 1 && tid != 2 && tid != 4 && tid != 6 && tid != 3 && tid != 11 && tid != 12) return;
        nlohmann::json obj;
        try { obj = nlohmann::json::parse(ld, ld + ll); } catch (...) { return; }

        if (tid == 4) {  // lap
            int lapNum = obj.value("lap_num", 0);
            if (curLapNum < 0) {
                curLapNum   = lapNum;
                int curMs   = obj.value("current_lap_ms", 0);
                curLapStart = curMs > 0 ? t - curMs / 1000.0f : t;
            } else if (lapNum > curLapNum) {
                auto it = lapBlocks_.find(curLapNum);
                if (it != lapBlocks_.end()) it->second.endSessionTime = t;
                int lapTimeMs = obj.value("last_lap_ms", 0);
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
                it = lapBlocks_.emplace(curLapNum, LapBlock{ curLapNum, t, t, {}, {} }).first;
            it->second.endSessionTime = t;
            if (tid == 1) {
                it->second.telemetry.push_back(obj);
            } else if (tid == 2) {
                it->second.statusHistory.push_back(obj);
            } else if (tid == 3) {
                it->second.damageHistory.push_back(obj);
            } else if (tid == 11) {
                it->second.motionHistory.push_back(obj);
            } else if (tid == 12) {
                it->second.motionExHistory.push_back(obj);
            }
        }

        if (tid == 6) {
            if (!obj.contains("session_time")) obj["session_time"] = t;
            scannedEvents_.push_back(obj);
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
    if (!partial.empty()) commitLine(partial.data(), (int)partial.size(), partialOffset);
    std::fclose(f);

    if (curLapNum >= 0) {
        auto it = lapBlocks_.find(curLapNum);
        if (it != lapBlocks_.end()) it->second.endSessionTime = totalTime_;
    }
    auto byT = [](const nlohmann::json& a, const nlohmann::json& b) {
        return a.value("session_time", 0.0f) < b.value("session_time", 0.0f);
    };
    for (auto& kv : lapBlocks_) {
        std::sort(kv.second.telemetry.begin(), kv.second.telemetry.end(), byT);
        std::sort(kv.second.statusHistory.begin(), kv.second.statusHistory.end(), byT);
    }
}

bool TnrdReader::load(const std::string& path, nlohmann::json& outHeader) {
    close();
    namespace fs = std::filesystem;
    auto tmpName = "tracknrace_" + std::to_string(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count()) + ".tmp";
    tempPath_ = (fs::temp_directory_path() / tmpName).string();

    if (!decompress(path, tempPath_)) { close(); return false; }

    {
        std::FILE* f = std::fopen(tempPath_.c_str(), "rb");
        if (!f) { close(); return false; }
        std::string line;
        char hb[8192];
        while (std::fgets(hb, sizeof(hb), f)) {
            line += hb;
            if (!line.empty() && line.back() == '\n') break;
            if (std::strlen(hb) < sizeof(hb) - 1) break;
        }
        std::fclose(f);
        try {
            outHeader = nlohmann::json::parse(line);
            if (!outHeader.contains("magic") || outHeader["magic"] != "TNRD_V1") { close(); return false; }
        } catch (...) { close(); return false; }
    }

    buildIndex(tempPath_);
    if (index_.empty()) { close(); return false; }

    tempFile_ = std::fopen(tempPath_.c_str(), "rb");
    if (!tempFile_) { close(); return false; }
    std::fseek(tempFile_, 0, SEEK_END);
    tempFileSize_ = std::ftell(tempFile_);
    return true;
}

void TnrdReader::close() {
    if (tempFile_) { std::fclose(tempFile_); tempFile_ = nullptr; }
    if (!tempPath_.empty()) {
        std::error_code ec;
        std::filesystem::remove(tempPath_, ec);
        tempPath_.clear();
    }
    index_.clear();
    lapBlocks_.clear();
    scannedLaps_.clear();
    scannedEvents_.clear();
    fastestLapNum_ = 0;
    fastestLapMs_  = 0;
    startTime_ = totalTime_ = 0.0f;
    tempFileSize_ = 0;
    playPos_ = 0;
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
}

std::vector<std::string> TnrdReader::stateSnapshot(float t) {
    std::vector<std::string> out;
    if (index_.empty()) return out;
    size_t pos = upperBoundTime(t);
    std::unordered_set<uint8_t> wanted(std::begin(STATE_TYPE_IDS), std::end(STATE_TYPE_IDS));
    std::vector<size_t> snapshot;
    for (size_t i = pos; i-- > 0 && !wanted.empty(); )
        if (wanted.erase(index_[i].type)) snapshot.push_back(i);
    std::sort(snapshot.begin(), snapshot.end());
    for (size_t idx : snapshot) {
        std::string s = readLine(index_[idx].offset);
        if (!s.empty()) out.push_back(std::move(s));
    }
    return out;
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
    for (const auto& l : scannedLaps_) {
        if (t >= l.startSessionTime && t <= l.endSessionTime) {
            startOut = l.startSessionTime;
            numOut   = l.lapNum;
            return true;
        }
    }
    return false;
}

nlohmann::json TnrdReader::lapBlocksMessage() const {
    nlohmann::json blocks = nlohmann::json::array();
    for (const auto& kv : lapBlocks_) {
        const LapBlock& b = kv.second;
        blocks.push_back({
            {"lapNum", b.lapNum},
            {"startSessionTime", b.startSessionTime},
            {"endSessionTime", b.endSessionTime},
        });
    }
    nlohmann::json laps = nlohmann::json::array();
    for (const auto& l : scannedLaps_)
        laps.push_back({ {"lapNum", l.lapNum}, {"lapTimeMs", l.lapTimeMs} });
    return {
        {"blocks", blocks},
        {"fastestLapNum", fastestLapNum_},
        {"events", scannedEvents_},
        {"laps", laps},
    };
}

nlohmann::json TnrdReader::getLapDataMessage(int lapNum) const {
    auto it = lapBlocks_.find(lapNum);
    if (it == lapBlocks_.end()) return nlohmann::json(nullptr);
    const LapBlock& b = it->second;
    return {
        {"lapNum", b.lapNum},
        {"startSessionTime", b.startSessionTime},
        {"endSessionTime", b.endSessionTime},
        {"telemetry", b.telemetry},
        {"statusHistory", b.statusHistory},
        {"motionHistory", b.motionHistory},
        {"motionExHistory", b.motionExHistory},
        {"damageHistory", b.damageHistory},
    };
}

std::vector<std::string> TnrdReader::pullUntil(float t) {
    std::vector<std::string> out;
    while (playPos_ < index_.size() && index_[playPos_].sessionTime <= t) {
        std::string s = readLine(index_[playPos_].offset);
        if (!s.empty()) out.push_back(std::move(s));
        ++playPos_;
    }
    return out;
}

std::vector<std::string> TnrdReader::drainRest() {
    std::vector<std::string> out;
    while (playPos_ < index_.size()) {
        std::string s = readLine(index_[playPos_].offset);
        if (!s.empty()) out.push_back(std::move(s));
        ++playPos_;
    }
    return out;
}

} // namespace tnrp
