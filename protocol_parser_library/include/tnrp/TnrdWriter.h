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
#include <functional>
#include <future>
#include <memory>

#include "tnrp/TnrdFormat.h"

namespace tnrp::detail { class TnrdOutputStream; }

namespace tnrp {

// Records parsed rows to compressed JSONL .tnrd files. TNRD V2/Zstandard is
// the default; TNRD V1/gzip remains available for legacy compatibility. Owns:
//   - per-session file rotation (new track/session => new file),
//   - a 30s rolling buffer so in-game flashbacks (<=30s) rewrite cleanly,
//   - rewind/flashback timeline truncation,
//   - per-type dedup of state rows.
//
// Not thread-safe; the engine serializes all calls.
class TnrdWriter {
public:
    using ErrorHandler = std::function<void(const std::string& operation,
                                            const std::string& message,
                                            const std::string& path)>;

    explicit TnrdWriter(ErrorHandler errorHandler = {});
    ~TnrdWriter();

    // Source-compatible default recording entry point: writes TNRD V2/zstd.
    void setLogging(bool enabled, const std::string& outputDir);
    void setLoggingZstd(bool enabled, const std::string& outputDir);
    [[deprecated("TNRD V1/gzip writing is retained only for compatibility; use setLoggingZstd")]]
    void setLoggingGzip(bool enabled, const std::string& outputDir);
    bool loggingEnabled() const { return wantRecord_; }

    // Cheap atomic mirror of "logging enabled" intent, updated synchronously in
    // setLogging(). The engine checks this before doing any per-packet recording
    // work (datagram copy, per-row json enqueue); when logging is off it skips the
    // whole pipeline. Mirrors intent rather than stream-open state so behaviour is
    // identical to before whenever logging is on (no dropped packets at session start).
    bool isRecording() const { return recording_.load(std::memory_order_relaxed); }

    // Called for every parsed packet selected for recording BEFORE its rows are
    // recorded. Handles flashback truncation and starts a new file when the
    // session packet reports a new track/session.
    void notePacket(uint16_t format, uint8_t packetId, float sessionTime,
                    const uint8_t* data, int length);

    // Append one serialised JSON row to the rolling buffer (deduped, flushed lazily).
    void record(const std::string& json, float sessionTime);

    // Synchronous writer-thread barriers. They first drain all events queued by
    // the UDP thread. flushToDisk keeps the stream open; closeActiveStream also
    // finalizes it. Both are safe to call from Engine control/shutdown threads.
    void flushToDisk();
    void closeActiveStream();

private:
    struct BufferEntry { std::string line; float sessionTime; };

    enum class EventType { SetLogging, NotePacket, Record, Flush, Close };

    struct WriterEvent {
        EventType             type;
        bool                  enabled;
        std::string           outputDir;
        TnrdFormat            tnrdFormat{TnrdFormat::ZstdV2};
        uint16_t              format;
        uint8_t               packetId;
        float                 sessionTime;
        std::vector<uint8_t>  packetData;
        std::string           json;   // serialised JSON row
        std::shared_ptr<std::promise<void>> completion;
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
    TnrdFormat  writeFormat_        = TnrdFormat::ZstdV2;
    std::string outputDirectory_;
    std::unique_ptr<detail::TnrdOutputStream> activeStream_;
    std::string activePath_;
    int         currentTrackId_     = -1;
    int         currentSessionType_ = -1;
    float       lastSessionTime_    = -1.0f;

    // Durability: force a codec flush on a cadence so a crash/power-loss leaves
    // a stream that is still decodable up to the last complete flushed row.
    int         rowsSinceFlush_     = 0;
    static constexpr int FLUSH_EVERY_ROWS = 300;  // ~5 s at buffered cadence

    std::vector<BufferEntry>                     rollingBuffer_;
    std::unordered_map<std::string, std::string> dedupeCache_;
    ErrorHandler                                  errorHandler_;
    std::string                                   lastReportedError_;

    static const std::unordered_set<std::string>& dedupeTypes();

    void startNewStream(int trackId, int sessionType, int format);
    bool flushBufferToDisk(const std::vector<BufferEntry>& entries);
    void flushToDiskOnWriterThread();
    void closeActiveStreamOnWriterThread();
    void flushOldBufferEntries();
    void truncateTimeline(float newSessionTime);
    bool isDuplicate(const std::string& type, const std::string& json);
    void reportError(const std::string& operation, const std::string& message,
                     const std::string& path);
    void clearReportedError();

    void setLoggingForFormat(bool enabled, const std::string& outputDir, TnrdFormat format);
};

} // namespace tnrp
