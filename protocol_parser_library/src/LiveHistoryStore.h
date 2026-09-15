#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tnrp::detail {

struct LiveHistoryJsonRow {
    float sessionTime{};
    uint64_t sequence{};
    std::shared_ptr<const std::string> json;
};

struct LiveHistoryBackfill {
    std::shared_ptr<std::vector<uint8_t>> binary;
    std::string json;
};

struct LiveHistoryMemoryStats {
    size_t retainedBytes{};
    size_t lapCount{};
    size_t pinnedLapCount{};
    size_t compressedLapCount{};
    size_t busyLapCount{};
    size_t packedBytes{};
    size_t packedCapacityBytes{};
    size_t jsonRows{};
    size_t jsonPayloadBytes{};
    size_t jsonPayloadCapacityBytes{};
    size_t jsonContainerCapacityBytes{};
    size_t sequenceEntries{};
    size_t sequenceCapacityBytes{};
    size_t compressedPlainBytes{};
    size_t compressedBytes{};
    size_t compressedCapacityBytes{};
    size_t queuedJobs{};
    int activeJobKind{}; // 0 idle, 1 compress, 2 decompress, 3 range
    uint64_t compressionJobs{};
    uint64_t compressedFamilies{};
    uint64_t compressionPlainBytesProcessed{};
    uint64_t compressionPlainBufferBytesAllocated{};
    uint64_t compressionBufferBytesAllocated{};
    uint64_t compressedOutputBytesAllocated{};
    size_t lastCompressionPlainBytes{};
    size_t lastCompressionBufferBytes{};
    size_t lastCompressionScratchBytes{};
    size_t peakCompressionPlainBytes{};
    size_t peakCompressionBufferBytes{};
    size_t peakCompressionScratchBytes{};
    uint64_t decompressionJobs{};
    uint64_t decompressionBufferBytesAllocated{};
    size_t lastDecompressionBufferBytes{};
    size_t peakDecompressionBufferBytes{};
    uint64_t rangeJobs{};
};

// Canonical live-session history. Rows are grouped by lap and family. Current,
// previous, previous-previous and fastest laps stay resident; older laps are
// compressed by the store's worker. Range requests run on that same worker so
// Electron's main thread and the UDP thread never perform decompression.
class LiveHistoryStore {
public:
    using BackfillCallback = std::function<void(LiveHistoryBackfill)>;

    LiveHistoryStore();
    ~LiveHistoryStore();
    LiveHistoryStore(const LiveHistoryStore&) = delete;
    LiveHistoryStore& operator=(const LiveHistoryStore&) = delete;

    void reset();
    void setLap(int lapNum, float startSessionTime, int completedLapTimeMs = 0);
    void appendPacked(uint8_t type, float sessionTime,
                      const uint8_t* data, size_t length);
    void appendJson(uint8_t type, LiveHistoryJsonRow row);
    void rewind(float sessionTime);

    int currentLap() const;
    float currentLapStart() const;
    std::string latestJson(uint8_t type, float throughSessionTime) const;
    LiveHistoryMemoryStats memoryStats() const;

    // Exceptional deep Strategy rollback: expand and release one lap at a time.
    // The memory callback must not acquire Engine locks (it can run under a
    // lap lock); arguments are temporary rows, JSON bytes and retained bytes.
    void forEachStrategyRow(float throughSessionTime,
        const std::function<void(const LiveHistoryJsonRow&)>& visitor,
        const std::function<void(size_t, size_t, size_t)>& memory = {}) const;

    // Callback runs on the history worker.
    void requestRange(uint32_t familyMask, float fromSessionTime,
                      float throughSessionTime, BackfillCallback callback);
    void requestFastestLap(std::function<void(int, int, float, float, LiveHistoryBackfill)> callback);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tnrp::detail
