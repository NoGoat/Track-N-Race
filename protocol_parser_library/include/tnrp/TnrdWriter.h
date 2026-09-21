#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <utility>
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
#include "tnrp/control_rows.h"

namespace tnrp::detail { class TnrdOutputStream; class TnrdV6Writer; }

namespace tnrp {

// Records parsed rows to .tnrd files. TNRD V6/chunked Zstandard is the default;
// TNRD V1/gzip remains available for legacy compatibility. Owns:
//   - per-session file rotation (new track/session => new file),
//   - a 30s rolling buffer so common short flashbacks avoid disk-side branching,
//   - V6 wall-clock branch cuts for append-only rewind/flashback recording,
//   - per-type dedup of state rows.
//
// Not thread-safe; the engine serializes all calls.
class TnrdWriter {
public:
    using ErrorHandler = std::function<void(const std::string& operation,
                                            const std::string& message,
                                            const std::string& path)>;

    struct MemoryStats {
        bool streamActive{};
        size_t retainedBytes{};
        size_t queuedEvents{};
        size_t queuedRetainedBytes{};
        size_t queuedRecordEvents{};
        size_t queuedNotePacketEvents{};
        size_t queuedControlEvents{};
        size_t queuedJsonBytes{};
        size_t queuedPacketBytes{};
        uint64_t oldestQueuedEventAgeMs{};
        size_t rollingEntries{};
        size_t rollingPayloadBytes{};
        size_t rollingPayloadCapacityBytes{};
        size_t rollingContainerCapacityBytes{};
        uint64_t rollingFlushBatches{};
        uint64_t rollingFlushEntriesProcessed{};
        uint64_t rollingFlushPayloadBytesProcessed{};
        size_t lastRollingFlushEntries{};
        size_t lastRollingFlushPayloadBytes{};
        size_t lastRollingFlushCopyCapacityBytes{};
        size_t peakRollingFlushEntries{};
        size_t peakRollingFlushPayloadBytes{};
        size_t peakRollingFlushCopyCapacityBytes{};
        uint64_t v5AppendBatches{};
        uint64_t v5AppendRowsProcessed{};
        uint64_t v5AppendPayloadBytesProcessed{};
        // Historical diagnostic key names retained for existing frontends;
        // these counters now describe the default V6 backend.
        size_t lastV5AppendRows{};
        size_t lastV5AppendPayloadBytes{};
        size_t lastV5SourceRowCapacityBytes{};
        size_t peakV5AppendRows{};
        size_t peakV5AppendPayloadBytes{};
        size_t peakV5SourceRowCapacityBytes{};
        size_t dedupeEntries{};
        size_t dedupePayloadBytes{};
        size_t dedupePayloadCapacityBytes{};
        size_t v5RetainedBytes{};
        size_t v5BuilderCount{};
        size_t v5BuilderPlainBytes{};
        size_t v5BuilderPlainCapacityBytes{};
        size_t v5BuilderRowIndexEntries{};
        size_t v5BuilderRowIndexCapacityBytes{};
        size_t v5ChunkCount{};
        size_t v5ChunkContainerCapacityBytes{};
        size_t v5ChunkRowIndexEntries{};
        size_t v5ChunkRowIndexCapacityBytes{};
        size_t v5BranchCount{};
        size_t v5BranchCapacityBytes{};
        size_t v5LapCount{};
        size_t v5StatusLapCount{};
        size_t v5EventCount{};
        size_t v5EventPayloadBytes{};
        size_t v5EventPayloadCapacityBytes{};
        size_t v5EventContainerCapacityBytes{};
        size_t v5LapStatusCapacityBytes{};
        uint64_t v5ChunkWrites{};
        uint64_t v5ChunkPlainBytesProcessed{};
        uint64_t v5ChunkCompressedBytesWritten{};
        uint64_t v5CompressionBufferBytesAllocated{};
        size_t v5CompressionScratchCapacityBytes{};
        size_t v5CompressionContextBytes{};
        size_t v5LastChunkPlainBytes{};
        size_t v5LastChunkCompressedBytes{};
        size_t v5LastCompressionBufferCapacityBytes{};
        size_t v5PeakCompressionBufferCapacityBytes{};
        uint64_t v5CheckpointWrites{};
        uint64_t v5CheckpointScratchBytesAllocated{};
        size_t v5LastCheckpointScratchBytes{};
        size_t v5PeakCheckpointScratchBytes{};
        size_t v5LastCheckpointDirectoryBytes{};
        size_t v5PeakCheckpointDirectoryBytes{};
        size_t v5LastCheckpointRowIndexBytes{};
        size_t v5PeakCheckpointRowIndexBytes{};
    };

    explicit TnrdWriter(ErrorHandler errorHandler = {});
    ~TnrdWriter();

    // Source-compatible default recording entry point: writes TNRD V6.
    void setLogging(bool enabled, const std::string& outputDir);
    void setLoggingZstd(bool enabled, const std::string& outputDir);
    // Zstandard level applied to V6 recordings opened after this call. Live
    // recording keeps the default; bulk offline conversion can afford more.
    void setCompressionLevel(int level);
    int compressionLevel() const { return compressionLevel_; }
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

    // Apply an authoritative FLBK target before recording the event/new V6 branch.
    void rewind(float sessionTime);

    // Append one serialised JSON row to the rolling buffer (deduped, flushed lazily).
    void record(const std::string& json, float sessionTime);

    // Synchronous writer-thread barriers. They first drain all events queued by
    // the UDP thread. flushToDisk keeps the stream open; closeActiveStream also
    // finalizes it. Both are safe to call from Engine control/shutdown threads.
    void flushToDisk();
    void closeActiveStream();
    MemoryStats memoryStats() const;

private:
    struct BufferEntry { std::string line; float sessionTime; };

    enum class EventType { SetLogging, Rewind, NotePacket, Record, Flush, Close };

    struct WriterEvent {
        EventType             type;
        bool                  enabled;
        std::string           outputDir;
        TnrdFormat            tnrdFormat{TnrdFormat::ChunkedV6};
        uint16_t              format;
        uint8_t               packetId;
        float                 sessionTime;
        uint64_t              wallClockMs{};
        uint64_t              queuedAtMs{};
        std::vector<uint8_t>  packetData;
        std::string           json;   // serialised JSON row
        std::shared_ptr<std::promise<void>> completion;
    };

    static constexpr float BUFFER_WINDOW_S = 30.0f;
    static constexpr uint64_t ROLLING_DRAIN_INTERVAL_MS = 250;
    static constexpr size_t ROLLING_DRAIN_ROW_THRESHOLD = 512;

    void writerLoop();

    mutable std::mutex      mu_;
    std::condition_variable cv_;
    std::queue<WriterEvent> queue_;
    size_t                  queuedRetainedBytes_{}; // guarded by mu_
    size_t                  queuedRecordEvents_{};  // guarded by mu_
    size_t                  queuedNotePacketEvents_{}; // guarded by mu_
    size_t                  queuedJsonBytes_{};     // guarded by mu_
    size_t                  queuedPacketBytes_{};   // guarded by mu_
    std::thread             diskThread_;
    std::atomic<bool>       stop_{false};
    std::atomic<bool>       recording_{false};  // mirrors "logging enabled" intent

    bool        wantRecord_         = false;
    int         compressionLevel_   = 3;
    TnrdFormat  writeFormat_        = TnrdFormat::ChunkedV6;
    std::string outputDirectory_;
    std::unique_ptr<detail::TnrdOutputStream> activeStream_;
    std::unique_ptr<detail::TnrdV6Writer> v6Writer_;
    std::string activePath_;
    int         currentTrackId_     = -1;
    int         currentSessionType_ = -1;
    float       lastSessionTime_    = -1.0f;
    float       v4LastCheckpointTime_ = -1.0f;

    // Durability: force a codec flush on a cadence so a crash/power-loss leaves
    // a stream that is still decodable up to the last complete flushed row.
    int         rowsSinceFlush_     = 0;
    static constexpr int FLUSH_EVERY_ROWS = 300;  // ~5 s at buffered cadence
    static constexpr float V4_CHECKPOINT_INTERVAL_S = 30.0f;

    std::deque<BufferEntry>                      rollingBuffer_;
    std::vector<std::pair<std::string_view, float>> v5SourceRowViews_;
    uint64_t                                      lastRollingDrainMs_{};
    std::unordered_map<std::string, std::string> dedupeCache_;
    ErrorHandler                                  errorHandler_;
    std::string                                   lastReportedError_;
    mutable std::mutex                            memoryStatsMutex_;
    MemoryStats                                   publishedMemoryStats_;
    uint64_t                                      lastMemoryStatsPublishMs_{};
    uint64_t                                      rollingFlushBatches_{};
    uint64_t                                      rollingFlushEntriesProcessed_{};
    uint64_t                                      rollingFlushPayloadBytesProcessed_{};
    size_t                                        lastRollingFlushEntries_{};
    size_t                                        lastRollingFlushPayloadBytes_{};
    size_t                                        lastRollingFlushCopyCapacityBytes_{};
    size_t                                        peakRollingFlushEntries_{};
    size_t                                        peakRollingFlushPayloadBytes_{};
    size_t                                        peakRollingFlushCopyCapacityBytes_{};
    uint64_t                                      v5AppendBatches_{};
    uint64_t                                      v5AppendRowsProcessed_{};
    uint64_t                                      v5AppendPayloadBytesProcessed_{};
    size_t                                        lastV5AppendRows_{};
    size_t                                        lastV5AppendPayloadBytes_{};
    size_t                                        lastV5SourceRowCapacityBytes_{};
    size_t                                        peakV5AppendRows_{};
    size_t                                        peakV5AppendPayloadBytes_{};
    size_t                                        peakV5SourceRowCapacityBytes_{};
    struct V5ActivityTotals {
        uint64_t chunkWrites{};
        uint64_t chunkPlainBytesProcessed{};
        uint64_t chunkCompressedBytesWritten{};
        uint64_t compressionBufferBytesAllocated{};
        size_t lastChunkPlainBytes{};
        size_t lastChunkCompressedBytes{};
        size_t lastCompressionBufferCapacityBytes{};
        size_t peakCompressionBufferCapacityBytes{};
        uint64_t checkpointWrites{};
        uint64_t checkpointScratchBytesAllocated{};
        size_t lastCheckpointScratchBytes{};
        size_t peakCheckpointScratchBytes{};
        size_t lastCheckpointDirectoryBytes{};
        size_t peakCheckpointDirectoryBytes{};
        size_t lastCheckpointRowIndexBytes{};
        size_t peakCheckpointRowIndexBytes{};
    } closedV5Activity_;

    static const std::unordered_set<std::string>& dedupeTypes();
    static size_t eventRetainedBytes(const WriterEvent& event);
    void pushEventLocked(WriterEvent event);
    void publishMemoryStatsOnWriterThread(bool force = false);

    void startNewStream(int trackId, int trackLengthM, int formula, int sessionType, int format);
    bool flushBufferToDisk(size_t entryCount, bool allowV4Checkpoint = true);
    void discardRollingPrefix(size_t entryCount);
    void flushToDiskOnWriterThread();
    void closeActiveStreamOnWriterThread();
    void flushOldBufferEntries();
    void truncateTimeline(float newSessionTime, uint64_t wallClockMs);
    bool isDuplicate(const std::string& type, const std::string& json);
    void reportError(const std::string& operation, const std::string& message,
                     const std::string& path);
    void clearReportedError();

    void setLoggingForFormat(bool enabled, const std::string& outputDir, TnrdFormat format);
    bool streamActive() const { return activeStream_ != nullptr || v6Writer_ != nullptr; }
};

} // namespace tnrp
