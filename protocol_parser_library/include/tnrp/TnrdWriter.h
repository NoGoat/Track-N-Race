#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>

#include <nlohmann/json.hpp>
#include <zlib.h>

namespace tnrp {

// Records parsed rows to gzip-compressed JSONL .tnrd files. Lifted from the
// native MainWindow recorder; Qt types replaced with std::string. Owns:
//   - per-session file rotation (new track/session => new file),
//   - a 30s rolling buffer so in-game flashbacks (<=30s) rewrite cleanly,
//   - rewind/flashback timeline truncation,
//   - per-type dedup of state rows.
//
// Not thread-safe; the engine serializes all calls.
class TnrdWriter {
public:
    TnrdWriter();
    ~TnrdWriter();

    // Enable/disable recording and set the output directory. Disabling closes
    // any active stream.
    void setLogging(bool enabled, const std::string& outputDir);
    bool loggingEnabled() const { return wantRecord_; }

    // Called for every accepted (non-rate-limited) packet BEFORE its rows are
    // recorded. Handles flashback truncation and starts a new file when the
    // session packet reports a new track/session. `data`/`length` is the raw
    // datagram (used to read trackId/sessionType from the session packet).
    void notePacket(uint16_t format, uint8_t packetId, float sessionTime,
                    const uint8_t* data, int length);

    // Append one parsed row to the rolling buffer (deduped, flushed lazily).
    void record(const nlohmann::json& row, float sessionTime);

    // Flush + close the active file (also called on session end / destruction).
    void closeActiveStream();

private:
    struct BufferEntry { std::string line; float sessionTime; };

    enum class EventType { SetLogging, NotePacket, Record, Close };

    struct WriterEvent {
        EventType type;
        bool enabled;
        std::string outputDir;
        uint16_t format;
        uint8_t packetId;
        float sessionTime;
        std::vector<uint8_t> packetData;
        nlohmann::json row;
    };

    static constexpr float BUFFER_WINDOW_S = 30.0f;

    void writerLoop();

    std::mutex              mu_;
    std::condition_variable cv_;
    std::queue<WriterEvent> queue_;
    std::thread             diskThread_;
    std::atomic<bool>       stop_{false};

    // State accessed only by diskThread_
    bool        wantRecord_        = false;
    std::string outputDirectory_;
    gzFile      activeGzip_        = nullptr;
    std::string activeGzipPath_;
    int         currentTrackId_    = -1;
    int         currentSessionType_= -1;
    float       lastSessionTime_   = -1.0f;

    std::vector<BufferEntry>                     rollingBuffer_;
    std::unordered_map<std::string, std::string> dedupeCache_;

    static const std::unordered_set<std::string>& dedupeTypes();

    void startNewStream(int trackId, int sessionType, int format);
    void flushBufferToDisk(const std::vector<BufferEntry>& entries);
    void flushOldBufferEntries();
    void truncateTimeline(float newSessionTime);
    bool isDuplicate(const std::string& type, const nlohmann::json& row);

    static gzFile gzOpenPath(const std::string& utf8Path, const char* mode);
};

} // namespace tnrp
