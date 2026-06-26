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

// State-defining packet types reconstructed on seek (the UI's panel state). The
// dense telemetry history and the append-only race_event log are not replayed.
static constexpr uint8_t RECON_TYPE_IDS[] = { 1, 2, 3, 4, 5, 7, 8, 9, 10 };

// Fast field extraction from a raw JSON line — avoids full JSON parsing during indexing.
static float scanSessionTime(const char* d, int len) {
    static const char KEY[] = "\"session_time\":";
    static constexpr int KLEN = sizeof(KEY) - 1;
    for (int i = 0; i <= len - KLEN; ++i) {
        if (d[i] == '"' && std::memcmp(d + i, KEY, KLEN) == 0)
            return std::strtof(d + i + KLEN, nullptr);
    }
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

TnrdReader::~TnrdReader() {
    close();
}

bool TnrdReader::decompress(const std::string& srcPath, const std::string& destPath) {
    gzFile gz = gzOpenRead(srcPath);
    if (!gz) return false;

    std::FILE* out = std::fopen(destPath.c_str(), "wb");
    if (!out) { gzclose(gz); return false; }

    char buf[131072];
    int n;
    bool ok = true;
    while ((n = gzread(gz, buf, sizeof(buf))) > 0) {
        if (std::fwrite(buf, 1, (size_t)n, out) != (size_t)n) { ok = false; break; }
    }
    if (n < 0) ok = false;
    gzclose(gz);
    std::fclose(out);
    return ok;
}

void TnrdReader::buildIndex(const std::string& filePath) {
    index_.clear();
    startTime_ = 0.0f;
    totalTime_ = 0.0f;
    playPos_   = 0;

    std::FILE* f = std::fopen(filePath.c_str(), "rb");
    if (!f) return;

    // Skip the header line.
    { int c; while ((c = std::fgetc(f)) != EOF && c != '\n') {} }

    constexpr long CHUNK = 2 * 1024 * 1024;
    std::vector<char> buf(CHUNK);
    std::string partial;            // incomplete line carried across chunks
    long partialOffset = std::ftell(f);
    float lastT = 0.0f;
    bool  haveStart = false;

    auto commitLine = [&](const char* ld, int ll, long lineOffset) {
        if (ll <= 1) return;
        float t = scanSessionTime(ld, ll);
        if (t < 0.0f) t = lastT;
        else          lastT = t;
        if (!haveStart) { startTime_ = t; haveStart = true; }
        uint8_t tid = scanType(ld, ll);
        index_.push_back({ lineOffset, t, tid });
        totalTime_ = std::max(totalTime_, t);
    };

    for (;;) {
        long chunkStart = std::ftell(f);
        size_t n = std::fread(buf.data(), 1, (size_t)CHUNK, f);
        if (n == 0) break;

        const char* base = buf.data();
        const char* p    = base;
        const char* end  = base + n;

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
                long lineOffset = chunkStart + (p - base);
                commitLine(p, (int)(nl - p), lineOffset);
            }
            p = nl + 1;
        }
    }

    if (!partial.empty())
        commitLine(partial.data(), (int)partial.size(), partialOffset);

    std::fclose(f);
}

bool TnrdReader::load(const std::string& path, nlohmann::json& outHeader) {
    close();

    namespace fs = std::filesystem;
    auto tmpName = "tracknrace_" + std::to_string(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count()) + ".tmp";
    tempPath_ = (fs::temp_directory_path() / tmpName).string();

    if (!decompress(path, tempPath_)) { close(); return false; }

    // Read + validate the header line.
    {
        std::FILE* f = std::fopen(tempPath_.c_str(), "rb");
        if (!f) { close(); return false; }
        std::string line;
        char buf[8192];
        while (std::fgets(buf, sizeof(buf), f)) {
            line += buf;
            if (!line.empty() && line.back() == '\n') break;
            if (std::strlen(buf) < sizeof(buf) - 1) break;
        }
        std::fclose(f);
        try {
            outHeader = nlohmann::json::parse(line);
            if (!outHeader.contains("magic") || outHeader["magic"] != "TNRD_V1") {
                close(); return false;
            }
        } catch (...) { close(); return false; }
    }

    buildIndex(tempPath_);
    if (index_.empty()) { close(); return false; }

    tempFile_ = std::fopen(tempPath_.c_str(), "rb");
    if (!tempFile_) { close(); return false; }
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
    startTime_ = 0.0f;
    totalTime_ = 0.0f;
    playPos_   = 0;
}

size_t TnrdReader::upperBoundTime(float t) const {
    size_t lo = 0, hi = index_.size();
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (index_[mid].sessionTime <= t) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

nlohmann::json TnrdReader::readLineAt(long offset) {
    if (!tempFile_) return {};
    if (std::fseek(tempFile_, offset, SEEK_SET) != 0) return {};
    std::string line;
    char buf[65536];
    while (std::fgets(buf, sizeof(buf), tempFile_)) {
        line += buf;
        if (!line.empty() && line.back() == '\n') break;
        if (std::strlen(buf) < sizeof(buf) - 1) break;
    }
    if (line.empty()) return {};
    try {
        return nlohmann::json::parse(line);
    } catch (...) {
        return {};
    }
}

std::vector<nlohmann::json> TnrdReader::seekToTime(float t) {
    std::vector<nlohmann::json> out;
    if (index_.empty()) return out;

    playPos_ = upperBoundTime(t);

    // Walk backward collecting the most recent occurrence of each state type.
    std::unordered_set<uint8_t> wanted(std::begin(RECON_TYPE_IDS), std::end(RECON_TYPE_IDS));
    std::vector<size_t> snapshot;
    for (size_t i = playPos_; i-- > 0 && !wanted.empty(); ) {
        if (wanted.erase(index_[i].type))
            snapshot.push_back(i);
    }
    std::sort(snapshot.begin(), snapshot.end());  // chronological
    for (size_t idx : snapshot) {
        nlohmann::json j = readLineAt(index_[idx].offset);
        if (!j.is_null()) out.push_back(std::move(j));
    }
    return out;
}

std::vector<nlohmann::json> TnrdReader::pullUntil(float t) {
    std::vector<nlohmann::json> out;
    while (playPos_ < index_.size() && index_[playPos_].sessionTime <= t) {
        nlohmann::json j = readLineAt(index_[playPos_].offset);
        if (!j.is_null()) out.push_back(std::move(j));
        ++playPos_;
    }
    return out;
}

std::vector<nlohmann::json> TnrdReader::drainRest() {
    std::vector<nlohmann::json> out;
    while (playPos_ < index_.size()) {
        nlohmann::json j = readLineAt(index_[playPos_].offset);
        if (!j.is_null()) out.push_back(std::move(j));
        ++playPos_;
    }
    return out;
}

} // namespace tnrp
