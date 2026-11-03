#pragma once

#include "TnrdIndexedArchive.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace tnrp::detail {

// Direct indexed V4 reader. open() reads and validates only the uncompressed
// control plane. Chunk payloads are decompressed on demand into a bounded LRU.
class TnrdV4Archive final : public TnrdIndexedArchive {
public:
    TnrdV4Archive();
    ~TnrdV4Archive();
    TnrdV4Archive(const TnrdV4Archive&) = delete;
    TnrdV4Archive& operator=(const TnrdV4Archive&) = delete;

    bool open(const std::string& path, HeaderRow& header, std::string* errorOut);
    void close();
    bool isOpen() const;
    const std::vector<V4LapInfo>& laps() const;
    const std::vector<V4ChunkInfo>& chunks() const;
    const V4ControlSummary& summary() const;
    float startTime() const;
    float totalTime() const;
    int lapAt(float sessionTime) const;

    void chunkIndicesForLap(uint32_t lap, V4RowTypeMask mask,
                            std::vector<size_t>& out) const;
    bool chunkTimeBounds(size_t chunkIndex, float& firstOut, float& lastOut) const;
    void prefetchChunk(size_t chunkIndex);
    void cancelPrefetch();
    bool rowsForChunks(const std::vector<size_t>& chunkIndices,
                       std::vector<std::vector<V4TimedRow>>& out,
                       std::string* errorOut);
    bool rowsForLap(uint32_t lap, V4RowTypeMask mask,
                    std::vector<V4TimedRow>& out, std::string* errorOut);
    bool rowsForLapRange(uint32_t lap, float fromTime, float toTime,
                         V4RowTypeMask mask, std::vector<V4TimedRow>& out,
                         std::string* errorOut, const IndexedCancelCheck& cancelled = {});
    bool rowsForRange(float fromTime, float toTime, V4RowTypeMask mask,
                      std::vector<V4TimedRow>& out, std::string* errorOut,
                      const IndexedCancelCheck& cancelled = {});
    bool latestRows(float atTime, const std::vector<uint8_t>& types,
                    std::vector<V4TimedRow>& out, std::string* errorOut,
                    const IndexedCancelCheck& cancelled = {});
    bool forEachChunk(V4RowTypeMask mask,
                      const std::function<bool(const V4ChunkInfo&, std::string_view)>& callback,
                      std::string* errorOut);

    void releaseTransientMemory() override;
    void setCacheLimitBytes(size_t bytes);
    size_t cacheBytes() const;
    uint64_t decompressedChunkCount() const;
    size_t peakConcurrentChunkLoads() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Append-only recording backend. Payload chunks and checkpoint tables are
// appended; existing telemetry bytes are never rewritten. Only the current
// lap's dirty row-family builders are recompressed at a checkpoint.
class TnrdV4Writer {
public:
    TnrdV4Writer();
    ~TnrdV4Writer();
    TnrdV4Writer(const TnrdV4Writer&) = delete;
    TnrdV4Writer& operator=(const TnrdV4Writer&) = delete;

    bool open(const std::string& path, const HeaderRow& header, std::string* errorOut);
    bool append(const std::vector<V4SourceRow>& rows, std::string* errorOut);
    bool checkpoint(std::string* errorOut);
    bool rewind(float sessionTime, std::string* errorOut);
    bool finish(std::string* errorOut);
    bool isOpen() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Finalizes a complete V4 container. Rows retain their logical JSON schema;
// the function partitions them by lap and row family and writes one independent
// checksummed Zstandard frame for each partition.
bool writeTnrdV4(const std::string& path, const HeaderRow& header,
                 const std::vector<V4SourceRow>& rows, std::string* errorOut);

struct V4LoadResult {
    HeaderRow header;
    std::unique_ptr<TnrdV4Archive> archive;
};

namespace TNRD_V4 {
bool load(const std::string& path, V4LoadResult& result, std::string& error);
}

} // namespace tnrp::detail
