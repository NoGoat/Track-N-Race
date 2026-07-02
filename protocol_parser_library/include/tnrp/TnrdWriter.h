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

    void setLogging(bool enabled, const std::string& outputDir);
    bool loggingEnabled() const { return wantRecord_; }

    // Cheap atomic mirror of "logging enabled" intent, updated synchronously in
    // setLogging(). The engine checks this before doing any per-packet recording
    // work (datagram copy, per-row json enqueue); when logging is off it skips the
    // whole pipeline. Mirrors intent rather than stream-open state so behaviour is
    // identical to before whenever logging is on (no dropped packets at session start).
    bool isRecording() const { return recording_.load(std::memory_order_relaxed); }

    // Called for every accepted (non-rate-limited) packet BEFORE its rows are
    // recorded. Handles flashback truncation and starts a new file when the
    // session packet reports a new track/session.
    void notePacket(uint16_t format, uint8_t packetId, float sessionTime,
                    const uint8_t* data, int length);

    // Append one serialised JSON row to the rolling buffer (deduped, flushed lazily).
    void record(const std::string& json, float sessionTime);

    void closeActiveStream();

private:
    struct BufferEntry { std::string line; float sessionTime; };

    enum class EventType { SetLogging, NotePacket, Record, Close };

    struct WriterEvent {
        EventType             type;
        bool                  enabled;
        std::string           outputDir;
        uint16_t              format;
        uint8_t               packetId;
        float                 sessionTime;
        std::vector<uint8_t>  packetData;
        std::string           json;   // serialised JSON row
    };

    static constexpr float BUFFER_WINDOW_S = 30.0f;

    void writerLoop();

    std::mutex              mu_;
    std::condition_variable cv_;
    std::queue<WriterEvent> queue_;
    std::thread             diskThread_;
    std::atomic<bool>       stop_{false};
    std::atomic<bool>       recording_{false};  // mirrors "logging enabled" intent

    bool        wantRecord_         = false;
    std::string outputDirectory_;
    gzFile      activeGzip_         = nullptr;
    std::string activeGzipPath_;
    int         currentTrackId_     = -1;
    int         currentSessionType_ = -1;
    float       lastSessionTime_    = -1.0f;

    // Durability: force a zlib sync point on a cadence so a crash/power-loss
    // mid-session leaves a stream that is still decodable up to the last flush.
    int         rowsSinceFlush_     = 0;
    static constexpr int FLUSH_EVERY_ROWS = 300;  // ~5 s at buffered cadence

    std::vector<BufferEntry>                     rollingBuffer_;
    std::unordered_map<std::string, std::string> dedupeCache_;

    static const std::unordered_set<std::string>& dedupeTypes();

    void startNewStream(int trackId, int sessionType, int format);
    void flushBufferToDisk(const std::vector<BufferEntry>& entries);
    void flushOldBufferEntries();
    void truncateTimeline(float newSessionTime);
    bool isDuplicate(const std::string& type, const std::string& json);

    static gzFile gzOpenPath(const std::string& utf8Path, const char* mode);
};

} // namespace tnrp
