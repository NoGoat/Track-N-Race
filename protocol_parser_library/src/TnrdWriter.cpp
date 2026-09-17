#include "tnrp/TnrdWriter.h"
#include "tnrp/Labels.h"
#include "tnrp/TimeUtils.h"
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
    // the reusable non-owning V5 staging array; payload capacities are below.
    stats.rollingContainerCapacityBytes = rollingBuffer_.size() * sizeof(BufferEntry) +
        v5SourceRowViews_.capacity() * sizeof(std::pair<std::string_view, float>);
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
    stats.v5AppendBatches = v5AppendBatches_;
    stats.v5AppendRowsProcessed = v5AppendRowsProcessed_;
    stats.v5AppendPayloadBytesProcessed = v5AppendPayloadBytesProcessed_;
    stats.lastV5AppendRows = lastV5AppendRows_;
    stats.lastV5AppendPayloadBytes = lastV5AppendPayloadBytes_;
    stats.lastV5SourceRowCapacityBytes = lastV5SourceRowCapacityBytes_;
    stats.peakV5AppendRows = peakV5AppendRows_;
    stats.peakV5AppendPayloadBytes = peakV5AppendPayloadBytes_;
    stats.peakV5SourceRowCapacityBytes = peakV5SourceRowCapacityBytes_;
    stats.dedupeEntries = dedupeCache_.size();
    for (const auto& [type, json] : dedupeCache_) {
        stats.dedupePayloadBytes += type.size() + json.size();
        stats.dedupePayloadCapacityBytes += type.capacity() + 1 + json.capacity() + 1;
    }
    stats.v5ChunkWrites = closedV5Activity_.chunkWrites;
    stats.v5ChunkPlainBytesProcessed = closedV5Activity_.chunkPlainBytesProcessed;
    stats.v5ChunkCompressedBytesWritten = closedV5Activity_.chunkCompressedBytesWritten;
    stats.v5CompressionBufferBytesAllocated = closedV5Activity_.compressionBufferBytesAllocated;
    stats.v5LastChunkPlainBytes = closedV5Activity_.lastChunkPlainBytes;
    stats.v5LastChunkCompressedBytes = closedV5Activity_.lastChunkCompressedBytes;
    stats.v5LastCompressionBufferCapacityBytes = closedV5Activity_.lastCompressionBufferCapacityBytes;
    stats.v5PeakCompressionBufferCapacityBytes = closedV5Activity_.peakCompressionBufferCapacityBytes;
    stats.v5CheckpointWrites = closedV5Activity_.checkpointWrites;
    stats.v5CheckpointScratchBytesAllocated = closedV5Activity_.checkpointScratchBytesAllocated;
    stats.v5LastCheckpointScratchBytes = closedV5Activity_.lastCheckpointScratchBytes;
    stats.v5PeakCheckpointScratchBytes = closedV5Activity_.peakCheckpointScratchBytes;
    stats.v5LastCheckpointDirectoryBytes = closedV5Activity_.lastCheckpointDirectoryBytes;
    stats.v5PeakCheckpointDirectoryBytes = closedV5Activity_.peakCheckpointDirectoryBytes;
    stats.v5LastCheckpointRowIndexBytes = closedV5Activity_.lastCheckpointRowIndexBytes;
    stats.v5PeakCheckpointRowIndexBytes = closedV5Activity_.peakCheckpointRowIndexBytes;
    if (v6Writer_) {
        const auto v5 = v6Writer_->memoryStats();
        stats.v5RetainedBytes = v5.retainedBytes;
        stats.v5BuilderCount = v5.builderCount;
        stats.v5BuilderPlainBytes = v5.builderPlainBytes;
        stats.v5BuilderPlainCapacityBytes = v5.builderPlainCapacityBytes;
        stats.v5BuilderRowIndexEntries = v5.builderRowIndexEntries;
        stats.v5BuilderRowIndexCapacityBytes = v5.builderRowIndexCapacityBytes;
        stats.v5ChunkCount = v5.chunkCount;
        stats.v5ChunkContainerCapacityBytes = v5.chunkContainerCapacityBytes;
        stats.v5ChunkRowIndexEntries = v5.chunkRowIndexEntries;
        stats.v5ChunkRowIndexCapacityBytes = v5.chunkRowIndexCapacityBytes;
        stats.v5BranchCount = v5.branchCount;
        stats.v5BranchCapacityBytes = v5.branchCapacityBytes;
        stats.v5LapCount = v5.lapCount;
        stats.v5StatusLapCount = v5.statusLapCount;
        stats.v5EventCount = v5.eventCount;
        stats.v5EventPayloadBytes = v5.eventPayloadBytes;
        stats.v5EventPayloadCapacityBytes = v5.eventPayloadCapacityBytes;
        stats.v5EventContainerCapacityBytes = v5.eventContainerCapacityBytes;
        stats.v5LapStatusCapacityBytes = v5.lapStatusCapacityBytes;
        stats.v5ChunkWrites += v5.chunkWrites;
        stats.v5ChunkPlainBytesProcessed += v5.chunkPlainBytesProcessed;
        stats.v5ChunkCompressedBytesWritten += v5.chunkCompressedBytesWritten;
        stats.v5CompressionBufferBytesAllocated += v5.compressionBufferBytesAllocated;
        stats.v5CompressionScratchCapacityBytes = v5.compressionScratchCapacityBytes;
        stats.v5CompressionContextBytes = v5.compressionContextBytes;
        stats.v5LastChunkPlainBytes = v5.lastChunkPlainBytes;
        stats.v5LastChunkCompressedBytes = v5.lastChunkCompressedBytes;
        stats.v5LastCompressionBufferCapacityBytes = v5.lastCompressionBufferCapacityBytes;
        stats.v5PeakCompressionBufferCapacityBytes = std::max(
            stats.v5PeakCompressionBufferCapacityBytes,
            v5.peakCompressionBufferCapacityBytes);
        stats.v5CheckpointWrites += v5.checkpointWrites;
        stats.v5CheckpointScratchBytesAllocated += v5.checkpointScratchBytesAllocated;
        stats.v5LastCheckpointScratchBytes = v5.lastCheckpointScratchBytes;
        stats.v5PeakCheckpointScratchBytes = std::max(
            stats.v5PeakCheckpointScratchBytes, v5.peakCheckpointScratchBytes);
        stats.v5LastCheckpointDirectoryBytes = v5.lastCheckpointDirectoryBytes;
        stats.v5PeakCheckpointDirectoryBytes = std::max(
            stats.v5PeakCheckpointDirectoryBytes, v5.peakCheckpointDirectoryBytes);
        stats.v5LastCheckpointRowIndexBytes = v5.lastCheckpointRowIndexBytes;
        stats.v5PeakCheckpointRowIndexBytes = std::max(
            stats.v5PeakCheckpointRowIndexBytes, v5.peakCheckpointRowIndexBytes);
    }
    stats.retainedBytes = stats.rollingPayloadCapacityBytes +
        stats.rollingContainerCapacityBytes + stats.dedupePayloadCapacityBytes +
        stats.v5RetainedBytes;
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

void TnrdWriter::setLoggingZstd(bool enabled, const std::string& outputDir) {
    setLoggingForFormat(enabled, outputDir, TnrdFormat::ChunkedV6);
}

void TnrdWriter::setLoggingGzip(bool enabled, const std::string& outputDir) {
    setLoggingForFormat(enabled, outputDir, TnrdFormat::GzipV1);
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
        } else if (ev.type == EventType::Rewind) {
            if (streamActive() && (lastSessionTime_ < 0.0f || ev.sessionTime < lastSessionTime_))
                truncateTimeline(ev.sessionTime, ev.wallClockMs);
        } else if (ev.type == EventType::NotePacket) {
            if (streamActive() && lastSessionTime_ >= 0.0f && ev.sessionTime < lastSessionTime_ - 0.2f)
                truncateTimeline(ev.sessionTime, wallClockMilliseconds());
            else if (ev.sessionTime > lastSessionTime_)
                lastSessionTime_ = ev.sessionTime;

            if (ev.packetId == PID_SESSION && ev.packetData.size() >= 708) {
                uint16_t trackLengthM = ReadUInt16(ev.packetData.data(), 33);
                int8_t  trackId     = ReadInt8(ev.packetData.data(), 36);
                uint8_t sessionType = ev.packetData[35];
                uint8_t formula     = ev.packetData[37];
                if (wantRecord_ && (trackId != currentTrackId_ ||
                                    sessionType != currentSessionType_ || !streamActive()))
                    startNewStream(trackId, trackLengthM, formula, sessionType, ev.format);
            }
        } else if (ev.type == EventType::Record) {
            if (!streamActive()) continue;
            std::string type = extractType(ev.json);
            if (isDuplicate(type, ev.json)) continue;
            const bool sessionEnd = type == "race_event" &&
                ev.json.find("\"code\":\"SEND\"") != std::string::npos;
            std::string line = std::move(ev.json);
            line.push_back('\n');
            float entryTime  = (ev.sessionTime >= 0.0f) ? ev.sessionTime : lastSessionTime_;
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
    if (v6Writer_) {
        if (flushBufferToDisk(rollingBuffer_.size(), false)) rollingBuffer_.clear();
        std::string err;
        if (!v6Writer_->checkpoint(&err)) reportError("checkpoint", err, activePath_);
        else v4LastCheckpointTime_ = lastSessionTime_;
        rowsSinceFlush_ = 0;
        return;
    }
    if (flushBufferToDisk(rollingBuffer_.size())) rollingBuffer_.clear();
    if (!activeStream_->flushRecoverable())
        reportError("flush", activeStream_->error(), activePath_);
    rowsSinceFlush_ = 0;
}

void TnrdWriter::closeActiveStreamOnWriterThread() {
    if (v6Writer_) {
        (void)flushBufferToDisk(rollingBuffer_.size(), false);
        std::string err;
        if (!v6Writer_->finish(&err)) reportError("close", err, activePath_);
        const auto v5 = v6Writer_->memoryStats();
        closedV5Activity_.chunkWrites += v5.chunkWrites;
        closedV5Activity_.chunkPlainBytesProcessed += v5.chunkPlainBytesProcessed;
        closedV5Activity_.chunkCompressedBytesWritten += v5.chunkCompressedBytesWritten;
        closedV5Activity_.compressionBufferBytesAllocated += v5.compressionBufferBytesAllocated;
        closedV5Activity_.lastChunkPlainBytes = v5.lastChunkPlainBytes;
        closedV5Activity_.lastChunkCompressedBytes = v5.lastChunkCompressedBytes;
        closedV5Activity_.lastCompressionBufferCapacityBytes =
            v5.lastCompressionBufferCapacityBytes;
        closedV5Activity_.peakCompressionBufferCapacityBytes = std::max(
            closedV5Activity_.peakCompressionBufferCapacityBytes,
            v5.peakCompressionBufferCapacityBytes);
        closedV5Activity_.checkpointWrites += v5.checkpointWrites;
        closedV5Activity_.checkpointScratchBytesAllocated +=
            v5.checkpointScratchBytesAllocated;
        closedV5Activity_.lastCheckpointScratchBytes = v5.lastCheckpointScratchBytes;
        closedV5Activity_.peakCheckpointScratchBytes = std::max(
            closedV5Activity_.peakCheckpointScratchBytes,
            v5.peakCheckpointScratchBytes);
        closedV5Activity_.lastCheckpointDirectoryBytes =
            v5.lastCheckpointDirectoryBytes;
        closedV5Activity_.peakCheckpointDirectoryBytes = std::max(
            closedV5Activity_.peakCheckpointDirectoryBytes,
            v5.peakCheckpointDirectoryBytes);
        closedV5Activity_.lastCheckpointRowIndexBytes =
            v5.lastCheckpointRowIndexBytes;
        closedV5Activity_.peakCheckpointRowIndexBytes = std::max(
            closedV5Activity_.peakCheckpointRowIndexBytes,
            v5.peakCheckpointRowIndexBytes);
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
    std::vector<std::pair<std::string_view, float>>().swap(v5SourceRowViews_);
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
    if (writeFormat_ == TnrdFormat::ChunkedV6) {
        v6Writer_ = std::make_unique<detail::TnrdV6Writer>();
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
    if (v6Writer_) {
        if (entryCount == 0) return true;
        v5SourceRowViews_.clear();
        v5SourceRowViews_.reserve(entryCount);
        size_t payloadBytes = 0;
        for (size_t index = 0; index < entryCount; ++index) {
            const auto& e = rollingBuffer_[index];
            v5SourceRowViews_.emplace_back(e.line, e.sessionTime);
            payloadBytes += e.line.size();
        }
        ++v5AppendBatches_;
        v5AppendRowsProcessed_ += entryCount;
        v5AppendPayloadBytesProcessed_ += payloadBytes;
        lastV5AppendRows_ = entryCount;
        lastV5AppendPayloadBytes_ = payloadBytes;
        lastV5SourceRowCapacityBytes_ = v5SourceRowViews_.capacity() *
            sizeof(std::pair<std::string_view, float>);
        peakV5AppendRows_ = std::max(peakV5AppendRows_, lastV5AppendRows_);
        peakV5AppendPayloadBytes_ = std::max(
            peakV5AppendPayloadBytes_, lastV5AppendPayloadBytes_);
        peakV5SourceRowCapacityBytes_ = std::max(
            peakV5SourceRowCapacityBytes_, lastV5SourceRowCapacityBytes_);
        std::string err;
        if (!v6Writer_->appendViews(v5SourceRowViews_, &err)) {
            v5SourceRowViews_.clear();
            reportError("data write", err, activePath_);
            return false;
        }
        v5SourceRowViews_.clear();
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

    if (v6Writer_) {
        rollingBuffer_.erase(
            std::remove_if(rollingBuffer_.begin(),rollingBuffer_.end(),
                [newSessionTime](const BufferEntry& e){return e.sessionTime>=newSessionTime;}),
            rollingBuffer_.end());
        if (newSessionTime < bufStart) {
            std::string err;
            if (!v6Writer_->rewind(newSessionTime,wallClockMs,&err))
                reportError("flashback",err,activePath_);
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
