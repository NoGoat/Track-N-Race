#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace tnrp {

// Reads a .tnrd file for playback. Qt-free port of the indexing core of the
// native TnrdPlayer: decompress to a temp file, build a time/type index with a
// fast byte scan (no full JSON parse during indexing), then stream rows on a
// clock the caller (Engine) owns.
//
// Unlike the native player it does NOT build a chart SessionModel during the
// scan — the bridge forwards raw rows and the renderer rebuilds its own state.
//
// Not thread-safe; the engine serializes access from its playback thread.
class TnrdReader {
public:
    TnrdReader() = default;
    ~TnrdReader();

    // Decompress + index. Returns false (and leaves nothing loaded) on a bad
    // file / missing TNRD_V1 magic. On success fills `outHeader`.
    bool load(const std::string& path, nlohmann::json& outHeader);
    void close();
    bool isLoaded() const { return tempFile_ != nullptr; }

    // Absolute session_time bounds of the recording.
    float startTime() const { return startTime_; }
    float totalTime() const { return totalTime_; }

    // Position the play cursor just after absolute time `t` and return the
    // reconstructed panel-state snapshot at `t` (most recent of each state type
    // at or before `t`), in chronological order. Dense history and the
    // append-only race_event log are intentionally not replayed.
    std::vector<nlohmann::json> seekToTime(float t);

    // Rows whose sessionTime is <= `t`, starting from the current cursor,
    // advancing the cursor past them.
    std::vector<nlohmann::json> pullUntil(float t);

    // True while the cursor hasn't reached the end of the index.
    bool hasMore() const { return playPos_ < index_.size(); }

    // Emit every remaining row (used to finish a clip cleanly).
    std::vector<nlohmann::json> drainRest();

private:
    struct IndexEntry {
        long    offset;
        float   sessionTime;
        uint8_t type;
    };

    std::vector<IndexEntry> index_;
    std::FILE*  tempFile_   = nullptr;   // decompressed JSONL, kept open for reads
    std::string tempPath_;
    float       startTime_  = 0.0f;
    float       totalTime_  = 0.0f;
    size_t      playPos_    = 0;

    size_t upperBoundTime(float t) const;  // first index with sessionTime > t

    static bool decompress(const std::string& srcPath, const std::string& destPath);
    void buildIndex(const std::string& filePath);
    nlohmann::json readLineAt(long offset);
};

} // namespace tnrp
