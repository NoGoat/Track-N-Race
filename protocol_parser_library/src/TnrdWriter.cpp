#include "tnrp/TnrdWriter.h"
#include "tnrp/Labels.h"
#include "tnrp/TimeUtils.h"
#include "tnrp/Capabilities.h"
#include "tnrp/control_rows.h"
#include "TnrdCodec.h"
#include "tnrd/TNRD_V1.h"
#include "tnrd/TNRD_V2.h"
#include "tnrd/TNRD_V3.h"
#include "tnrd/TNRD_V6.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <utility>

#include "protocols/protocol.h"

#ifdef _WIN32
#  include <windows.h>
#endif

namespace tnrp {

static constexpr int PID_SESSION = 1;
static constexpr int PID_CAR_TEL2 = 16;

// Pulls just session_time out of a stored row for flashback truncation. A row
// without the key keeps the default 0.0f, so it is always kept (matching the
// previous "no session_time => keep" behaviour for non-negative cutoffs).
// Must have external linkage (not in an anonymous namespace): glaze's
// compile-time reflection takes the type's mangled name, which MSVC refuses
// to do for internal-linkage types (error C7631). GCC/Clang allow it.
struct SessionTimeOnly { float session_time{}; };
namespace {
constexpr glz::opts kPartialReadW{ .null_terminated = false, .error_on_unknown_keys = false };

uint64_t wallClockMilliseconds() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::unique_ptr<detail::TnrdOutputStream> openVersionWriter(
        TnrdFormat format, const std::string& path, bool append, std::string& error) {
    switch (format) {
        case TnrdFormat::GzipV1: return detail::TNRD_V1::openWriter(path, append, error);
        case TnrdFormat::ZstdV2: return detail::TNRD_V2::openWriter(path, append, error);
        case TnrdFormat::ZstdV3: return detail::TNRD_V3::openWriter(path, append, error);
        default: error = "The selected TNRD format does not use a streaming writer."; return nullptr;
    }
}

void prepareVersionHeader(TnrdFormat format, HeaderRow& header) {
    switch (format) {
        case TnrdFormat::GzipV1: detail::TNRD_V1::prepareHeader(header); break;
        case TnrdFormat::ZstdV2: detail::TNRD_V2::prepareHeader(header); break;
        case TnrdFormat::ZstdV3: detail::TNRD_V3::prepareHeader(header); break;
        default: break; // Indexed formats stamp metadata in their writer's open().
    }
}
}

static std::string extractType(const std::string& json) {
    static const char KEY[] = "\"type\":\"";
    static constexpr int KLEN = sizeof(KEY) - 1;
    auto pos = json.find(KEY);
    if (pos == std::string::npos) return {};
    pos += KLEN;
    auto end = json.find('"', pos);
    if (end == std::string::npos) return {};
    return json.substr(pos, end - pos);
}

const std::unordered_set<std::string>& TnrdWriter::dedupeTypes() {
    static const std::unordered_set<std::string> kTypes = {
        "session", "tyre_sets", "participants", "all_status", "timing", "damage"
    };
    return kTypes;
}

size_t TnrdWriter::eventRetainedBytes(const WriterEvent& event) {
    return sizeof(WriterEvent) + event.outputDir.capacity() + 1 +
        event.packetData.capacity() + event.json.capacity() + 1;
}

void TnrdWriter::pushEventLocked(WriterEvent event) {
    event.queuedAtMs = wallClockMilliseconds();
    queuedRetainedBytes_ += eventRetainedBytes(event);
    queuedJsonBytes_ += event.json.size();
    queuedPacketBytes_ += event.packetData.size();
    if (event.type == EventType::Record) ++queuedRecordEvents_;
    if (event.type == EventType::NotePacket) ++queuedNotePacketEvents_;
    queue_.push(std::move(event));
}

TnrdWriter::MemoryStats TnrdWriter::memoryStats() const {
    MemoryStats stats;
    {
        std::lock_guard<std::mutex> lock(memoryStatsMutex_);
        stats = publishedMemoryStats_;
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        stats.queuedEvents = queue_.size();
        stats.queuedRetainedBytes = queuedRetainedBytes_;
        stats.queuedRecordEvents = queuedRecordEvents_;
        stats.queuedNotePacketEvents = queuedNotePacketEvents_;
        stats.queuedControlEvents = stats.queuedEvents >=
                stats.queuedRecordEvents + stats.queuedNotePacketEvents
            ? stats.queuedEvents - stats.queuedRecordEvents - stats.queuedNotePacketEvents
            : 0;
        stats.queuedJsonBytes = queuedJsonBytes_;
        stats.queuedPacketBytes = queuedPacketBytes_;
        if (!queue_.empty() && queue_.front().queuedAtMs != 0) {
            const uint64_t now = wallClockMilliseconds();
            stats.oldestQueuedEventAgeMs = now >= queue_.front().queuedAtMs
                ? now - queue_.front().queuedAtMs : 0;
        }
    }
    stats.retainedBytes += stats.queuedRetainedBytes;
    return stats;
}

void TnrdWriter::publishMemoryStatsOnWriterThread(bool force) {
    const uint64_t now = wallClockMilliseconds();
    if (!force && lastMemoryStatsPublishMs_ != 0 &&
        now - lastMemoryStatsPublishMs_ < 900) return;

    MemoryStats stats;
    stats.streamActive = streamActive();
    stats.rollingEntries = rollingBuffer_.size();
    // std::deque does not expose block capacity. Count live entry storage plus
    // the reusable non-owning V6 staging array; payload capacities are below.
    stats.rollingContainerCapacityBytes = rollingBuffer_.size() * sizeof(BufferEntry) +
        v6SourceRowViews_.capacity() * sizeof(std::pair<std::string_view, float>);
    for (const auto& entry : rollingBuffer_) {
        stats.rollingPayloadBytes += entry.line.size();
        stats.rollingPayloadCapacityBytes += entry.line.capacity() + 1;
    }
    stats.rollingFlushBatches = rollingFlushBatches_;
    stats.rollingFlushEntriesProcessed = rollingFlushEntriesProcessed_;
    stats.rollingFlushPayloadBytesProcessed = rollingFlushPayloadBytesProcessed_;
    stats.lastRollingFlushEntries = lastRollingFlushEntries_;
    stats.lastRollingFlushPayloadBytes = lastRollingFlushPayloadBytes_;
    stats.lastRollingFlushCopyCapacityBytes = lastRollingFlushCopyCapacityBytes_;
    stats.peakRollingFlushEntries = peakRollingFlushEntries_;
    stats.peakRollingFlushPayloadBytes = peakRollingFlushPayloadBytes_;
    stats.peakRollingFlushCopyCapacityBytes = peakRollingFlushCopyCapacityBytes_;
    stats.v6AppendBatches = v6AppendBatches_;
    stats.v6AppendRowsProcessed = v6AppendRowsProcessed_;
    stats.v6AppendPayloadBytesProcessed = v6AppendPayloadBytesProcessed_;
    stats.lastV6AppendRows = lastV6AppendRows_;
    stats.lastV6AppendPayloadBytes = lastV6AppendPayloadBytes_;
    stats.lastV6SourceRowCapacityBytes = lastV6SourceRowCapacityBytes_;
    stats.peakV6AppendRows = peakV6AppendRows_;
    stats.peakV6AppendPayloadBytes = peakV6AppendPayloadBytes_;
    stats.peakV6SourceRowCapacityBytes = peakV6SourceRowCapacityBytes_;
    stats.dedupeEntries = dedupeCache_.size();
    for (const auto& [type, json] : dedupeCache_) {
        stats.dedupePayloadBytes += type.size() + json.size();
        stats.dedupePayloadCapacityBytes += type.capacity() + 1 + json.capacity() + 1;
    }
    stats.v6ChunkWrites = closedV6Activity_.chunkWrites;
    stats.v6ChunkPlainBytesProcessed = closedV6Activity_.chunkPlainBytesProcessed;
    stats.v6ChunkCompressedBytesWritten = closedV6Activity_.chunkCompressedBytesWritten;
    stats.v6CompressionBufferBytesAllocated = closedV6Activity_.compressionBufferBytesAllocated;
    stats.v6LastChunkPlainBytes = closedV6Activity_.lastChunkPlainBytes;
    stats.v6LastChunkCompressedBytes = closedV6Activity_.lastChunkCompressedBytes;
    stats.v6LastCompressionBufferCapacityBytes = closedV6Activity_.lastCompressionBufferCapacityBytes;
    stats.v6PeakCompressionBufferCapacityBytes = closedV6Activity_.peakCompressionBufferCapacityBytes;
    stats.v6CheckpointWrites = closedV6Activity_.checkpointWrites;
    stats.v6CheckpointScratchBytesAllocated = closedV6Activity_.checkpointScratchBytesAllocated;
    stats.v6LastCheckpointScratchBytes = closedV6Activity_.lastCheckpointScratchBytes;
    stats.v6PeakCheckpointScratchBytes = closedV6Activity_.peakCheckpointScratchBytes;
    stats.v6LastCheckpointDirectoryBytes = closedV6Activity_.lastCheckpointDirectoryBytes;
    stats.v6PeakCheckpointDirectoryBytes = closedV6Activity_.peakCheckpointDirectoryBytes;
    stats.v6LastCheckpointRowIndexBytes = closedV6Activity_.lastCheckpointRowIndexBytes;
    stats.v6PeakCheckpointRowIndexBytes = closedV6Activity_.peakCheckpointRowIndexBytes;
    if (v6Writer_) {
        const auto v6 = v6Writer_->memoryStats();
        stats.v6RetainedBytes = v6.retainedBytes;
        stats.v6BuilderCount = v6.builderCount;
        stats.v6BuilderPlainBytes = v6.builderPlainBytes;
        stats.v6BuilderPlainCapacityBytes = v6.builderPlainCapacityBytes;
        stats.v6BuilderRowIndexEntries = v6.builderRowIndexEntries;
        stats.v6BuilderRowIndexCapacityBytes = v6.builderRowIndexCapacityBytes;
        stats.v6ChunkCount = v6.chunkCount;
        stats.v6ChunkContainerCapacityBytes = v6.chunkContainerCapacityBytes;
        stats.v6ChunkRowIndexEntries = v6.chunkRowIndexEntries;
        stats.v6ChunkRowIndexCapacityBytes = v6.chunkRowIndexCapacityBytes;
        stats.v6BranchCount = v6.branchCount;
        stats.v6BranchCapacityBytes = v6.branchCapacityBytes;
        stats.v6LapCount = v6.lapCount;
        stats.v6StatusLapCount = v6.statusLapCount;
        stats.v6EventCount = v6.eventCount;
        stats.v6EventPayloadBytes = v6.eventPayloadBytes;
        stats.v6EventPayloadCapacityBytes = v6.eventPayloadCapacityBytes;
        stats.v6EventContainerCapacityBytes = v6.eventContainerCapacityBytes;
        stats.v6LapStatusCapacityBytes = v6.lapStatusCapacityBytes;
        stats.v6ChunkWrites += v6.chunkWrites;
        stats.v6ChunkPlainBytesProcessed += v6.chunkPlainBytesProcessed;
        stats.v6ChunkCompressedBytesWritten += v6.chunkCompressedBytesWritten;
        stats.v6CompressionBufferBytesAllocated += v6.compressionBufferBytesAllocated;
        stats.v6CompressionScratchCapacityBytes = v6.compressionScratchCapacityBytes;
        stats.v6CompressionContextBytes = v6.compressionContextBytes;
        stats.v6LastChunkPlainBytes = v6.lastChunkPlainBytes;
        stats.v6LastChunkCompressedBytes = v6.lastChunkCompressedBytes;
        stats.v6LastCompressionBufferCapacityBytes = v6.lastCompressionBufferCapacityBytes;
        stats.v6PeakCompressionBufferCapacityBytes = std::max(
            stats.v6PeakCompressionBufferCapacityBytes,
            v6.peakCompressionBufferCapacityBytes);
        stats.v6CheckpointWrites += v6.checkpointWrites;
        stats.v6CheckpointScratchBytesAllocated += v6.checkpointScratchBytesAllocated;
        stats.v6LastCheckpointScratchBytes = v6.lastCheckpointScratchBytes;
        stats.v6PeakCheckpointScratchBytes = std::max(
            stats.v6PeakCheckpointScratchBytes, v6.peakCheckpointScratchBytes);
        stats.v6LastCheckpointDirectoryBytes = v6.lastCheckpointDirectoryBytes;
        stats.v6PeakCheckpointDirectoryBytes = std::max(
            stats.v6PeakCheckpointDirectoryBytes, v6.peakCheckpointDirectoryBytes);
        stats.v6LastCheckpointRowIndexBytes = v6.lastCheckpointRowIndexBytes;
        stats.v6PeakCheckpointRowIndexBytes = std::max(
            stats.v6PeakCheckpointRowIndexBytes, v6.peakCheckpointRowIndexBytes);
        stats.sessionRetained = retainSession_;
        stats.sessionFileAttached = v6Writer_->hasFile();
        stats.v6PendingLapCount = v6.pendingLapCount;
        stats.v6PendingLapBytes = v6.pendingLapPlainBytes;
        stats.v6MemoryChunkBytes = v6.memoryChunkBytes;
        stats.v6MemorySharedRecords = v6.memorySharedRecords;
        stats.v6MemorySharedBytes = v6.memorySharedBytes;
        stats.v6UncommittedLaps = v6.uncommittedLaps;
    }
    stats.retainedBytes = stats.rollingPayloadCapacityBytes +
        stats.rollingContainerCapacityBytes + stats.dedupePayloadCapacityBytes +
        stats.v6RetainedBytes;
    {
        std::lock_guard<std::mutex> lock(memoryStatsMutex_);
        publishedMemoryStats_ = stats;
    }
    lastMemoryStatsPublishMs_ = now;
}

TnrdWriter::TnrdWriter(ErrorHandler errorHandler)
    : errorHandler_(std::move(errorHandler)) {
    diskThread_ = std::thread(&TnrdWriter::writerLoop, this);
}

void TnrdWriter::reportError(const std::string& operation, const std::string& message,
                             const std::string& path) {
    const std::string key = operation + '\n' + message + '\n' + path;
    if (key == lastReportedError_) return;
    lastReportedError_ = key;
    std::fprintf(stderr, "[tnrd] writer %s failed for '%s': %s\n",
                 operation.c_str(), path.c_str(), message.c_str());
    if (errorHandler_) {
        try {
            errorHandler_(operation, message, path);
        } catch (...) {
            // Error reporting must never terminate the recording disk thread.
        }
    }
}

void TnrdWriter::clearReportedError() {
    lastReportedError_.clear();
}

TnrdWriter::~TnrdWriter() {
    {
        std::unique_lock<std::mutex> lk(mu_);
        WriterEvent ev;
        ev.type = EventType::Close;
        pushEventLocked(std::move(ev));
        stop_.store(true);
    }
    cv_.notify_all();
    if (diskThread_.joinable()) diskThread_.join();
}

void TnrdWriter::flushToDisk() {
    auto completion = std::make_shared<std::promise<void>>();
    auto done = completion->get_future();
    {
        std::unique_lock<std::mutex> lk(mu_);
        WriterEvent ev;
        ev.type = EventType::Flush;
        ev.completion = std::move(completion);
        pushEventLocked(std::move(ev));
    }
    cv_.notify_one();
    done.wait();
}

void TnrdWriter::closeActiveStream() {
    auto completion = std::make_shared<std::promise<void>>();
    auto done = completion->get_future();
    {
        std::unique_lock<std::mutex> lk(mu_);
        WriterEvent ev;
        ev.type = EventType::Close;
        ev.completion = std::move(completion);
        pushEventLocked(std::move(ev));
    }
    cv_.notify_one();
    done.wait();
}

void TnrdWriter::setLogging(bool enabled, const std::string& outputDir) {
    setLoggingZstd(enabled, outputDir);
}

void TnrdWriter::setCompressionLevel(int level) { compressionLevel_ = level; }
void TnrdWriter::setLoggingZstd(bool enabled, const std::string& outputDir) {
    setLoggingForFormat(enabled, outputDir, TnrdFormat::ChunkedV6);
}

void TnrdWriter::setLoggingGzip(bool enabled, const std::string& outputDir) {
    setLoggingForFormat(enabled, outputDir, TnrdFormat::GzipV1);
}

void TnrdWriter::setRetainSession(bool retain) {
    retainRequested_.store(retain, std::memory_order_relaxed);
    {
        std::unique_lock<std::mutex> lk(mu_);
        WriterEvent ev;
        ev.type = EventType::SetRetain;
        ev.enabled = retain;
        pushEventLocked(std::move(ev));
    }
    cv_.notify_one();
}

void TnrdWriter::resetSession() {
    {
        std::unique_lock<std::mutex> lk(mu_);
        WriterEvent ev;
        ev.type = EventType::ResetSession;
        pushEventLocked(std::move(ev));
    }
    cv_.notify_one();
}

void TnrdWriter::requestMemoryImage(const detail::V6ImageFilter& filter, MemoryImageCallback done) {
    {
        std::unique_lock<std::mutex> lk(mu_);
        WriterEvent ev;
        ev.type = EventType::MemoryImage;
        ev.imageFilter = std::make_shared<const detail::V6ImageFilter>(filter);
        ev.imageDone = std::move(done);
        pushEventLocked(std::move(ev));
    }
    cv_.notify_one();
}

bool TnrdWriter::streamActive() const {
    return activeStream_ != nullptr ||
        (v6Writer_ != nullptr && (!retainSession_ || v6Writer_->hasFile()));
}

// The retained session's memory writer, opened with the session's first packet.
void TnrdWriter::ensureSessionWriter(uint16_t format) {
    if (v6Writer_) {
        if (format != 0) v6Writer_->setProtocol(format);
        return;
    }
    HeaderRow header;
    header.protocol = format;
    auto writer = std::make_unique<detail::TnrdV6Writer>();
    writer->setCompressionLevel(compressionLevel_);
    std::string error;
    if (!writer->openMemory(header, &error)) { reportError("open", error, {}); return; }
    v6Writer_ = std::move(writer);
}

// The retained session's file was closed where a write failed. The session
// carries on; the next session packet starts another file holding all of it.
void TnrdWriter::noteV6FileError() {
    if (!retainSession_ || !v6Writer_) return;
    const std::string error = v6Writer_->takeFileError();
    if (error.empty()) return;
    reportError("data write", error, activePath_);
    if (!activeStream_) activePath_.clear();
}

// Drops the retained session for a new one, finishing its file. A damaged
// session (a rewind it could not take) leaves its file as it stands for the
// reader's recovery scan instead of indexing what memory now holds.
void TnrdWriter::dropSession(bool damaged) {
    if (v6Writer_) {
        const bool hadFile = v6Writer_->hasFile();
        std::string error;
        if (damaged) v6Writer_->abort();
        else if (!v6Writer_->finish(&error)) reportError("close", error, activePath_);
        accumulateV6Activity();
        v6Writer_.reset();
        if (hadFile && !activeStream_) {
            activePath_.clear();
            currentTrackId_ = -1;
            currentSessionType_ = -1;
        }
    }
    sessionLatest_ = -std::numeric_limits<float>::infinity();
    sessionEnded_ = false;
}

void TnrdWriter::accumulateV6Activity() {
    if (!v6Writer_) return;
    const auto v6 = v6Writer_->memoryStats();
    closedV6Activity_.chunkWrites += v6.chunkWrites;
    closedV6Activity_.chunkPlainBytesProcessed += v6.chunkPlainBytesProcessed;
    closedV6Activity_.chunkCompressedBytesWritten += v6.chunkCompressedBytesWritten;
    closedV6Activity_.compressionBufferBytesAllocated += v6.compressionBufferBytesAllocated;
    closedV6Activity_.lastChunkPlainBytes = v6.lastChunkPlainBytes;
    closedV6Activity_.lastChunkCompressedBytes = v6.lastChunkCompressedBytes;
    closedV6Activity_.lastCompressionBufferCapacityBytes =
        v6.lastCompressionBufferCapacityBytes;
    closedV6Activity_.peakCompressionBufferCapacityBytes = std::max(
        closedV6Activity_.peakCompressionBufferCapacityBytes,
        v6.peakCompressionBufferCapacityBytes);
    closedV6Activity_.checkpointWrites += v6.checkpointWrites;
    closedV6Activity_.checkpointScratchBytesAllocated +=
        v6.checkpointScratchBytesAllocated;
    closedV6Activity_.lastCheckpointScratchBytes = v6.lastCheckpointScratchBytes;
    closedV6Activity_.peakCheckpointScratchBytes = std::max(
        closedV6Activity_.peakCheckpointScratchBytes,
        v6.peakCheckpointScratchBytes);
    closedV6Activity_.lastCheckpointDirectoryBytes =
        v6.lastCheckpointDirectoryBytes;
    closedV6Activity_.peakCheckpointDirectoryBytes = std::max(
        closedV6Activity_.peakCheckpointDirectoryBytes,
        v6.peakCheckpointDirectoryBytes);
    closedV6Activity_.lastCheckpointRowIndexBytes =
        v6.lastCheckpointRowIndexBytes;
    closedV6Activity_.peakCheckpointRowIndexBytes = std::max(
        closedV6Activity_.peakCheckpointRowIndexBytes,
        v6.peakCheckpointRowIndexBytes);
}

void TnrdWriter::setLoggingForFormat(bool enabled, const std::string& outputDir,
                                     TnrdFormat format) {
    recording_.store(enabled, std::memory_order_relaxed);
    std::unique_lock<std::mutex> lk(mu_);
    WriterEvent ev;
    ev.type = EventType::SetLogging;
    ev.enabled = enabled;
    ev.outputDir = outputDir;
    ev.tnrdFormat = format;
    pushEventLocked(std::move(ev));
    cv_.notify_one();
}

void TnrdWriter::notePacket(uint16_t format, uint8_t packetId, float sessionTime,
                            const uint8_t* data, int length) {
    std::unique_lock<std::mutex> lk(mu_);
    if (!data || length <= 0) return;
    WriterEvent ev;
    ev.type = EventType::NotePacket;
    ev.format = format;
    ev.packetId = packetId;
    ev.sessionTime = sessionTime;
    // Only session bytes are needed here for recording rotation. The protocol
    // parsers have already extracted all measurement fields into rows.
    if (packetId == PID_SESSION)
        ev.packetData.assign(data, data + length);
    // The recording header keeps which regulations the cars follow.
    if (format == 2026 && packetId == PID_CAR_TEL2)
        ev.regulations2026 = regulations2026FromCarTelemetry2(data, length);
    pushEventLocked(std::move(ev));
    cv_.notify_one();
}

void TnrdWriter::rewind(float sessionTime) {
    if (!std::isfinite(sessionTime) || sessionTime < 0.0f) return;
    std::unique_lock<std::mutex> lk(mu_);
    WriterEvent ev;
    ev.type = EventType::Rewind;
    ev.sessionTime = sessionTime;
    ev.wallClockMs = wallClockMilliseconds();
    pushEventLocked(std::move(ev));
    cv_.notify_one();
}

void TnrdWriter::record(const std::string& json, float sessionTime) {
    std::unique_lock<std::mutex> lk(mu_);
    WriterEvent ev;
    ev.type = EventType::Record;
    ev.json = json;
    ev.sessionTime = sessionTime;
    pushEventLocked(std::move(ev));
    cv_.notify_one();
}

void TnrdWriter::writerLoop() {
    while (true) {
        WriterEvent ev;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [this] { return !queue_.empty(); });
            const size_t retainedBytes = eventRetainedBytes(queue_.front());
            const size_t jsonBytes = queue_.front().json.size();
            const size_t packetBytes = queue_.front().packetData.size();
            const EventType eventType = queue_.front().type;
            ev = std::move(queue_.front());
            queue_.pop();
            queuedRetainedBytes_ = retainedBytes <= queuedRetainedBytes_
                ? queuedRetainedBytes_ - retainedBytes : 0;
            queuedJsonBytes_ = jsonBytes <= queuedJsonBytes_
                ? queuedJsonBytes_ - jsonBytes : 0;
            queuedPacketBytes_ = packetBytes <= queuedPacketBytes_
                ? queuedPacketBytes_ - packetBytes : 0;
            if (eventType == EventType::Record && queuedRecordEvents_ > 0)
                --queuedRecordEvents_;
            if (eventType == EventType::NotePacket && queuedNotePacketEvents_ > 0)
                --queuedNotePacketEvents_;
        }

        if (ev.type == EventType::SetLogging) {
            const bool formatChanged = wantRecord_ && writeFormat_ != ev.tnrdFormat;
            const bool directoryChanged = wantRecord_ && outputDirectory_ != ev.outputDir;
            wantRecord_ = ev.enabled;
            outputDirectory_ = ev.outputDir;
            writeFormat_ = ev.tnrdFormat;
            clearReportedError();
            // A live directory change must rotate away from the already-open
            // file. The next session packet starts a new recording in the new
            // directory, instead of requiring an application restart.
            if (!ev.enabled || formatChanged || directoryChanged)
                closeActiveStreamOnWriterThread();
        } else if (ev.type == EventType::SetRetain) {
            // Only before a session or file exists; the engine sets it first.
            if (!v6Writer_) retainSession_ = ev.enabled;
        } else if (ev.type == EventType::ResetSession) {
            if (retainSession_) dropSession(false);
        } else if (ev.type == EventType::MemoryImage) {
            std::shared_ptr<const detail::V6MemoryImage> image;
            if (retainSession_ && v6Writer_ && ev.imageFilter)
                image = v6Writer_->memoryImage(*ev.imageFilter);
            if (ev.imageDone) ev.imageDone(std::move(image));
        } else if (ev.type == EventType::Rewind) {
            const bool retained = retainSession_ && v6Writer_;
            if (retained && ev.sessionTime < sessionLatest_) {
                std::string error;
                if (!v6Writer_->rewind(ev.sessionTime, &error)) {
                    // Memory reopens committed laps, so this is a damaged
                    // session; it starts afresh.
                    reportError("flashback", error, activePath_);
                    dropSession(true);
                } else {
                    noteV6FileError();
                }
                sessionLatest_ = ev.sessionTime;
            }
            // A file of its own (a legacy stream, or V6 without a retained
            // session) rewinds its own timeline.
            const bool ownFile = activeStream_ || (!retainSession_ && v6Writer_);
            if (ownFile && (lastSessionTime_ < 0.0f || ev.sessionTime < lastSessionTime_))
                truncateTimeline(ev.sessionTime, ev.wallClockMs);
            else if (retained && lastSessionTime_ > ev.sessionTime)
                lastSessionTime_ = ev.sessionTime;
        } else if (ev.type == EventType::NotePacket) {
            if (activeStream_ && lastSessionTime_ >= 0.0f && ev.sessionTime < lastSessionTime_ - 0.2f)
                truncateTimeline(ev.sessionTime, wallClockMilliseconds());
            else if (ev.sessionTime > lastSessionTime_)
                lastSessionTime_ = ev.sessionTime;

            if (retainSession_) {
                ensureSessionWriter(ev.format);
                if (std::isfinite(ev.sessionTime)) sessionLatest_ = std::max(sessionLatest_, ev.sessionTime);
            }
            if (v6Writer_) {
                std::string error;
                if (!v6Writer_->advanceSessionTime(ev.sessionTime, &error))
                    reportError("session-time advance", error, activePath_);
                if (ev.regulations2026) v6Writer_->setRegulations2026(*ev.regulations2026);
                noteV6FileError();
            }

            if (ev.packetId == PID_SESSION && ev.packetData.size() >= 708) {
                uint16_t trackLengthM = ReadUInt16(ev.packetData.data(), 33);
                int8_t  trackId     = ReadInt8(ev.packetData.data(), 36);
                uint8_t sessionType = ev.packetData[35];
                uint8_t formula     = ev.packetData[37];
                if (wantRecord_ && !(retainSession_ && sessionEnded_) &&
                    (trackId != currentTrackId_ || sessionType != currentSessionType_ || !streamActive()))
                    startNewStream(trackId, trackLengthM, formula, sessionType, ev.format);
            }
        } else if (ev.type == EventType::Record) {
            if (!streamActive() && !(retainSession_ && v6Writer_)) continue;
            std::string type = extractType(ev.json);
            const bool sessionEnd = type == "race_event" &&
                ev.json.find("\"code\":\"SEND\"") != std::string::npos;
            const float entryTime = (ev.sessionTime >= 0.0f) ? ev.sessionTime : lastSessionTime_;
            // V6 is an exact packet-history format: retain every parsed row at
            // the UDP cadence, including unchanged 10 Hz damage packets. A
            // retained session takes every row whether or not it is recorded.
            if (v6Writer_) {
                std::string error;
                if (!v6Writer_->appendRow(ev.json, entryTime, &error))
                    reportError("data write", error, activePath_);
                noteV6FileError();
                if (sessionEnd && retainSession_) sessionEnded_ = true;
                // A legacy stream beside a retained session closes both below.
                if (!activeStream_) {
                    if (sessionEnd && streamActive()) {
                        closeActiveStreamOnWriterThread();
                        publishMemoryStatsOnWriterThread(true);
                    }
                    continue;
                }
            }
            // The legacy formats keep their historical state-row deduplication
            // and their readers reconstruct the omitted cadence during playback.
            if (isDuplicate(type, ev.json)) continue;
            std::string line = std::move(ev.json);
            line.push_back('\n');
            rollingBuffer_.push_back({std::move(line), entryTime});
            if (sessionEnd) {
                closeActiveStreamOnWriterThread();
                publishMemoryStatsOnWriterThread(true);
                continue;
            }
            flushOldBufferEntries();
        } else if (ev.type == EventType::Flush) {
            flushToDiskOnWriterThread();
            if (ev.completion) ev.completion->set_value();
        } else if (ev.type == EventType::Close) {
            closeActiveStreamOnWriterThread();
            if (ev.completion) ev.completion->set_value();
            if (stop_.load() && queue_.empty()) break;
        }
        publishMemoryStatsOnWriterThread(ev.type == EventType::SetLogging ||
                                         ev.type == EventType::Flush ||
                                         ev.type == EventType::Close);
    }
}

void TnrdWriter::flushToDiskOnWriterThread() {
    if (!streamActive()) return;
    if (v6Writer_ && v6Writer_->hasFile()) {
        std::string err;
        if (!v6Writer_->checkpoint(&err)) reportError("checkpoint", err, activePath_);
        else v4LastCheckpointTime_ = lastSessionTime_;
        noteV6FileError();
    }
    if (activeStream_) {
        if (flushBufferToDisk(rollingBuffer_.size())) rollingBuffer_.clear();
        if (!activeStream_->flushRecoverable())
            reportError("flush", activeStream_->error(), activePath_);
    }
    rowsSinceFlush_ = 0;
}

void TnrdWriter::closeActiveStreamOnWriterThread() {
    if (v6Writer_ && retainSession_) {
        // The session stays in memory; only its file is finished.
        std::string err;
        if (v6Writer_->hasFile() && !v6Writer_->detachFile(&err)) reportError("close", err, activePath_);
    } else if (v6Writer_) {
        std::string err;
        if (!v6Writer_->finish(&err)) reportError("close", err, activePath_);
        accumulateV6Activity();
        v6Writer_.reset();
    }
    if (activeStream_) {
        (void)flushBufferToDisk(rollingBuffer_.size());
        if (!activeStream_->finish())
            reportError("close", activeStream_->error(), activePath_);
        activeStream_.reset();
    }
    // A session can end with the entire 30-second rewind window still queued.
    // Release its deque blocks and borrowed-view capacity at rotation/close so
    // the recorder itself does not pin that session's high-water allocation.
    std::deque<BufferEntry>().swap(rollingBuffer_);
    std::vector<std::pair<std::string_view, float>>().swap(v6SourceRowViews_);
    currentTrackId_     = -1;
    currentSessionType_ = -1;
    activePath_.clear();
    lastSessionTime_    = -1.0f;
    rowsSinceFlush_     = 0;
    v4LastCheckpointTime_ = -1.0f;
    lastRollingDrainMs_ = 0;
    dedupeCache_.clear();
}

void TnrdWriter::startNewStream(int trackId, int trackLengthM, int formula, int sessionType, int format) {
    closeActiveStreamOnWriterThread();
    if (!wantRecord_ || outputDirectory_.empty()) return;

    const std::string proto = RecordingFilenamePrefix(format);

    auto itTrack = TRACK_NAMES.find(trackId);
    const std::string trackNameKey = "track." + std::to_string(trackId) + ".track_name";
    const std::string trackNameOverride = labelsFor((uint16_t)format).get(trackNameKey);
    const std::string resolvedTrackName = trackNameOverride != trackNameKey
        ? trackNameOverride
        : (itTrack != TRACK_NAMES.end() ? itTrack->second : "Unknown");
    std::string tName = resolvedTrackName != "Unknown"
        ? sanitizeName(resolvedTrackName) : "track_" + std::to_string(trackId);

    auto itSess = SESSION_NAMES.find(sessionType);
    std::string sName = (itSess != SESSION_NAMES.end())
        ? sanitizeName(itSess->second) : "session_" + std::to_string(sessionType);

    std::string filename = proto + "_" + std::to_string(trackId) + "_"
                         + tName + "_" + sName + "_" + filenameTimestamp() + ".tnrd";

    activePath_ = outputDirectory_ + "/" + filename;
    HeaderRow hdr;
    hdr.protocol     = format;
    hdr.track_id     = trackId;
    hdr.track_name   = resolvedTrackName;
    hdr.track_length_m = trackLengthM;
    if (writeFormat_ == TnrdFormat::ChunkedV6) hdr.formula = formula;
    hdr.session_type = sessionType;
    hdr.session_name = (itSess != SESSION_NAMES.end()) ? itSess->second : "Unknown";
    hdr.start_time   = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    prepareVersionHeader(writeFormat_, hdr);

    std::string openError;
    if (writeFormat_ == TnrdFormat::ChunkedV6 && retainSession_) {
        // The recording joins the session kept in memory: the file starts
        // with everything held so far, then takes each commit with it.
        if (!v6Writer_) openError = "there is no live session to record";
        else (void)v6Writer_->attachFile(activePath_, hdr, &openError);
    } else if (writeFormat_ == TnrdFormat::ChunkedV6) {
        v6Writer_ = std::make_unique<detail::TnrdV6Writer>();
        v6Writer_->setCompressionLevel(compressionLevel_);
        if (!v6Writer_->open(activePath_, hdr, &openError)) v6Writer_.reset();
    } else {
        activeStream_ = openVersionWriter(writeFormat_, activePath_, false, openError);
    }

    if (streamActive()) {
        clearReportedError();
        std::string hl = writeJson(hdr) + "\n";
        if (activeStream_ && !activeStream_->write(hl)) {
            reportError("header write", activeStream_->error(), activePath_);
            activeStream_.reset();
            activePath_.clear();
            return;
        }

        currentTrackId_     = trackId;
        currentSessionType_ = sessionType;
        lastSessionTime_    = -1.0f;
        rowsSinceFlush_     = 0;
        v4LastCheckpointTime_ = -1.0f;
    } else {
        reportError("open", openError, activePath_);
        activePath_.clear();
    }
}

bool TnrdWriter::flushBufferToDisk(size_t entryCount, bool allowV4Checkpoint) {
    entryCount = std::min(entryCount, rollingBuffer_.size());
    // A retained session's writer never takes the legacy stream's rows.
    if (v6Writer_ && !retainSession_) {
        if (entryCount == 0) return true;
        v6SourceRowViews_.clear();
        v6SourceRowViews_.reserve(entryCount);
        size_t payloadBytes = 0;
        for (size_t index = 0; index < entryCount; ++index) {
            const auto& e = rollingBuffer_[index];
            v6SourceRowViews_.emplace_back(e.line, e.sessionTime);
            payloadBytes += e.line.size();
        }
        ++v6AppendBatches_;
        v6AppendRowsProcessed_ += entryCount;
        v6AppendPayloadBytesProcessed_ += payloadBytes;
        lastV6AppendRows_ = entryCount;
        lastV6AppendPayloadBytes_ = payloadBytes;
        lastV6SourceRowCapacityBytes_ = v6SourceRowViews_.capacity() *
            sizeof(std::pair<std::string_view, float>);
        peakV6AppendRows_ = std::max(peakV6AppendRows_, lastV6AppendRows_);
        peakV6AppendPayloadBytes_ = std::max(
            peakV6AppendPayloadBytes_, lastV6AppendPayloadBytes_);
        peakV6SourceRowCapacityBytes_ = std::max(
            peakV6SourceRowCapacityBytes_, lastV6SourceRowCapacityBytes_);
        std::string err;
        if (!v6Writer_->appendViews(v6SourceRowViews_, &err)) {
            v6SourceRowViews_.clear();
            reportError("data write", err, activePath_);
            return false;
        }
        v6SourceRowViews_.clear();
        const float newestTime = rollingBuffer_[entryCount - 1].sessionTime;
        if (allowV4Checkpoint && (v4LastCheckpointTime_ < 0.0f ||
            newestTime - v4LastCheckpointTime_ >= V4_CHECKPOINT_INTERVAL_S)) {
            if (!v6Writer_->checkpoint(&err)) {
                reportError("checkpoint", err, activePath_);
                // appendViews() already copied these rows into V6 builders.
                // Keep recording without duplicating them in the rolling
                // buffer; the next checkpoint retries pending state.
                return true;
            }
            v4LastCheckpointTime_ = newestTime;
        }
        return true;
    }
    if (!activeStream_ || entryCount == 0) return true;
    for (size_t index = 0; index < entryCount; ++index) {
        const auto& e = rollingBuffer_[index];
        if (!activeStream_->write(e.line)) {
            reportError("data write", activeStream_->error(), activePath_);
            return false;
        }
    }

    // Periodically emit a codec-specific recoverability point. Both zlib's sync
    // flush and Zstandard's stream flush make all complete rows supplied so far
    // immediately decodable without ending the active member/frame.
    rowsSinceFlush_ += static_cast<int>(entryCount);
    if (rowsSinceFlush_ >= FLUSH_EVERY_ROWS) {
        if (!activeStream_->flushRecoverable())
            reportError("flush", activeStream_->error(), activePath_);
        rowsSinceFlush_ = 0;
    }
    return true;
}

void TnrdWriter::discardRollingPrefix(size_t entryCount) {
    entryCount = std::min(entryCount, rollingBuffer_.size());
    while (entryCount-- > 0) rollingBuffer_.pop_front();
}

void TnrdWriter::flushOldBufferEntries() {
    if (lastSessionTime_ < 0.0f || rollingBuffer_.empty()) return;
    float cutoff = lastSessionTime_ - BUFFER_WINDOW_S;
    size_t flush = 0;
    while (flush < rollingBuffer_.size() && rollingBuffer_[flush].sessionTime < cutoff)
        flush++;
    if (flush > 0) {
        const uint64_t now = wallClockMilliseconds();
        const bool intervalElapsed = lastRollingDrainMs_ == 0 || now < lastRollingDrainMs_ ||
            now - lastRollingDrainMs_ >= ROLLING_DRAIN_INTERVAL_MS;
        if (flush < ROLLING_DRAIN_ROW_THRESHOLD && !intervalElapsed) return;
        size_t payloadBytes = 0;
        for (size_t index = 0; index < flush; ++index) {
            const auto& entry = rollingBuffer_[index];
            payloadBytes += entry.line.size();
        }
        ++rollingFlushBatches_;
        rollingFlushEntriesProcessed_ += flush;
        rollingFlushPayloadBytesProcessed_ += payloadBytes;
        lastRollingFlushEntries_ = flush;
        lastRollingFlushPayloadBytes_ = payloadBytes;
        // Rows are now borrowed in place; no payload-copy allocation occurs.
        lastRollingFlushCopyCapacityBytes_ = 0;
        peakRollingFlushEntries_ = std::max(peakRollingFlushEntries_, flush);
        peakRollingFlushPayloadBytes_ = std::max(
            peakRollingFlushPayloadBytes_, payloadBytes);
        if (flushBufferToDisk(flush)) {
            discardRollingPrefix(flush);
            lastRollingDrainMs_ = now;
        }
    }
}

bool TnrdWriter::isDuplicate(const std::string& type, const std::string& json) {
    if (!dedupeTypes().count(type)) return false;
    // Parse only for deduped state types to strip volatile fields before hashing.
    // Player status is intentionally not deduped: its menu-rate sample timestamps
    // are chart history and must survive recording even when values hold steady.
    glz::generic doc;
    if (glz::read_json(doc, json)) return false;
    if (doc.is_object()) {
        doc.get_object().erase("ts");
        doc.get_object().erase("session_time");
    }
    std::string hash;
    if (glz::write_json(doc, hash)) return false;
    auto it = dedupeCache_.find(type);
    if (it != dedupeCache_.end() && it->second == hash) return true;
    dedupeCache_[type] = std::move(hash);
    return false;
}

void TnrdWriter::truncateTimeline(float newSessionTime, uint64_t wallClockMs) {
    float bufStart = rollingBuffer_.empty()
        ? std::numeric_limits<float>::infinity() : rollingBuffer_[0].sessionTime;

    if (v6Writer_ && !retainSession_) {
        std::string err;
        if (!v6Writer_->rewind(newSessionTime,&err)) {
            reportError("flashback",err,activePath_);
            v6Writer_->abort(); v6Writer_.reset(); activePath_.clear();
        }
        dedupeCache_.clear();lastSessionTime_=newSessionTime;return;
    }

    if (newSessionTime >= bufStart) {
        rollingBuffer_.erase(
            std::remove_if(rollingBuffer_.begin(), rollingBuffer_.end(),
                [newSessionTime](const BufferEntry& e) { return e.sessionTime > newSessionTime; }),
            rollingBuffer_.end());
    } else {
        rollingBuffer_.clear();
        if (!activePath_.empty() && activeStream_) {
            if (!activeStream_->finish())
                reportError("close before flashback", activeStream_->error(), activePath_);
            activeStream_.reset();

            std::vector<std::string> kept;
            const std::string plainPath = activePath_ + ".rewrite.jsonl.tmp";
            const std::string compressedPath = activePath_ + ".rewrite.tnrd.tmp";
            bool partial = false;
            std::string codecError;
            const bool decompressed = detail::decompressTnrd(
                activePath_, plainPath, writeFormat_, &partial, &codecError);
            if (decompressed) {
#ifdef _WIN32
                std::ifstream in(std::filesystem::path(detail::windowsExtendedPath(plainPath)),
                                 std::ios::binary);
#else
                std::ifstream in(std::filesystem::u8path(plainPath), std::ios::binary);
#endif
                std::string line;
                while (std::getline(in, line)) {
                    line.push_back('\n');
                    SessionTimeOnly st;
                    (void)glz::read<kPartialReadW>(st, line);
                    if (st.session_time <= newSessionTime)
                        kept.push_back(line);
                }
            } else {
                reportError("flashback decompress", codecError, activePath_);
            }

            bool replaced = false;
            codecError.clear();
            auto rewritten = decompressed && !kept.empty()
                ? openVersionWriter(writeFormat_, compressedPath, false, codecError)
                : nullptr;
            if (rewritten) {
                bool writeOk = true;
                for (const auto& line : kept) {
                    if (!rewritten->write(line)) { writeOk = false; break; }
                }
                writeOk = rewritten->finish() && writeOk;
                rewritten.reset();
                if (writeOk) {
                    std::error_code ec;
#ifdef _WIN32
                    const auto src = detail::windowsExtendedPath(compressedPath);
                    const auto dst = detail::windowsExtendedPath(activePath_);
                    replaced = MoveFileExW(src.c_str(), dst.c_str(),
                                           MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
                    std::filesystem::rename(compressedPath, activePath_, ec);
                    replaced = !ec;
#endif
                }
            }
            if (!replaced)
                reportError("flashback rewrite",
                            codecError.empty() ? "could not replace the original recording" : codecError,
                            activePath_);

            std::error_code cleanupError;
#ifdef _WIN32
            std::filesystem::remove(
                std::filesystem::path(detail::windowsExtendedPath(plainPath)), cleanupError);
            std::filesystem::remove(
                std::filesystem::path(detail::windowsExtendedPath(compressedPath)), cleanupError);
#else
            std::filesystem::remove(std::filesystem::u8path(plainPath), cleanupError);
            std::filesystem::remove(std::filesystem::u8path(compressedPath), cleanupError);
#endif

            codecError.clear();
            activeStream_ = openVersionWriter(writeFormat_, activePath_, true, codecError);
            if (!activeStream_)
                reportError("flashback append reopen", codecError, activePath_);
        }
    }
    dedupeCache_.clear();
    lastSessionTime_ = newSessionTime;
}

} // namespace tnrp
