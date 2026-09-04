#pragma once

#include "TNRD_V4.h"

namespace tnrp::detail {

// V5 retains V4's indexed-container table shapes. The aliases keep the shared
// query interface stable while the V5 reader/writer implementation remains
// independent and validates only V5 identities.
using V5SourceRow = V4SourceRow;
using V5LapInfo = V4LapInfo;
using V5ChunkInfo = V4ChunkInfo;
using V5LapStatusSummary = V4LapStatusSummary;
using V5ControlSummary = V4ControlSummary;
using V5TimedRow = V4TimedRow;
using V5RowTypeMask = V4RowTypeMask;
constexpr V5RowTypeMask v5TypeBit(uint8_t type) { return v4TypeBit(type); }

class TnrdV5Archive final : public TnrdIndexedArchive {
public:
    TnrdV5Archive();
    ~TnrdV5Archive();
    TnrdV5Archive(const TnrdV5Archive&) = delete;
    TnrdV5Archive& operator=(const TnrdV5Archive&) = delete;

    bool open(const std::string& path, HeaderRow& header, std::string* errorOut);
    void close();
    bool isOpen() const;
    const std::vector<V5LapInfo>& laps() const;
    const std::vector<V5ChunkInfo>& chunks() const;
    const V5ControlSummary& summary() const;
    float startTime() const;
    float totalTime() const;
    int lapAt(float sessionTime) const;

    void chunkIndicesForLap(uint32_t lap, V5RowTypeMask mask,
                            std::vector<size_t>& out) const;
    bool chunkTimeBounds(size_t chunkIndex, float& firstOut, float& lastOut) const;
    void prefetchChunk(size_t chunkIndex);
    void cancelPrefetch();
    bool rowsForChunks(const std::vector<size_t>& chunkIndices,
                       std::vector<std::vector<V5TimedRow>>& out,
                       std::string* errorOut);
    // Playback-frontier variant. The selected chunks are still decompressed as
    // whole Zstandard frames, but their row indexes avoid materializing rows
    // outside the requested time window.
    bool rowsForChunksRange(const std::vector<size_t>& chunkIndices,
                            float fromTime, float toTime,
                            std::vector<std::vector<V5TimedRow>>& out,
                            std::string* errorOut,
                            const IndexedCancelCheck& cancelled = {});
    bool rowsForLap(uint32_t lap, V5RowTypeMask mask,
                    std::vector<V5TimedRow>& out, std::string* errorOut);
    bool rowsForLapRange(uint32_t lap, float fromTime, float toTime,
                         V5RowTypeMask mask, std::vector<V5TimedRow>& out,
                         std::string* errorOut, const IndexedCancelCheck& cancelled = {});
    bool rowsForRange(float fromTime, float toTime, V5RowTypeMask mask,
                      std::vector<V5TimedRow>& out, std::string* errorOut,
                      const IndexedCancelCheck& cancelled = {});
    bool latestRows(float atTime, const std::vector<uint8_t>& types,
                    std::vector<V5TimedRow>& out, std::string* errorOut,
                    const IndexedCancelCheck& cancelled = {});
    bool forEachChunk(V5RowTypeMask mask,
                      const std::function<bool(const V5ChunkInfo&, std::string_view)>& callback,
                      std::string* errorOut);

    void releaseTransientMemory();
    void setCacheLimitBytes(size_t bytes);
    size_t cacheBytes() const;
    uint64_t decompressedChunkCount() const;
    size_t peakConcurrentChunkLoads() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class TnrdV5Writer {
public:
    TnrdV5Writer();
    ~TnrdV5Writer();
    TnrdV5Writer(const TnrdV5Writer&) = delete;
    TnrdV5Writer& operator=(const TnrdV5Writer&) = delete;

    bool open(const std::string& path, const HeaderRow& header, std::string* errorOut);
    bool append(const std::vector<V5SourceRow>& rows, std::string* errorOut);
    bool checkpoint(std::string* errorOut);
    bool rewind(float sessionTime, std::string* errorOut);
    bool finish(std::string* errorOut);
    bool isOpen() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

bool writeTnrdV5(const std::string& path, const HeaderRow& header,
                 const std::vector<V5SourceRow>& rows, std::string* errorOut);

struct V5LoadResult {
    HeaderRow header;
    std::unique_ptr<TnrdV5Archive> archive;
};

namespace TNRD_V5 {
bool load(const std::string& path, V5LoadResult& result, std::string& error);
}

} // namespace tnrp::detail
