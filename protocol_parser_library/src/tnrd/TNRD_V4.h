#pragma once

#include <cstdint>
#include <cstddef>
#include <memory>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "tnrp/control_rows.h"

namespace tnrp::detail {

struct V4SourceRow {
    std::string line;
    float sessionTime{};
};

struct V4LapInfo {
    uint32_t lapNumber{};
    float startSessionTime{};
    float endSessionTime{};
    uint32_t lapTimeMs{};
    uint32_t flags{};
};

struct V4ChunkInfo {
    uint32_t lapNumber{};
    uint16_t rowType{};
    uint16_t flags{};
    uint64_t offset{};
    uint64_t compressedSize{};
    uint64_t uncompressedSize{};
    uint32_t rowCount{};
    uint32_t checksum{};
    uint64_t sequence{};
};

struct V4LapStatusSummary {
    uint32_t lapNumber{};
    float sessionTime{};
    double ersPct{};
    int tyreCompound{};
    int visualCompound{};
};

struct V4ControlSummary {
    double initialFuelKg{-1.0};
    float startSessionTime{};
    float totalSessionTime{};
    std::vector<std::string> events;
    std::vector<V4LapStatusSummary> lapStatus;
};

struct V4TimedRow {
    float sessionTime{};
    uint8_t rowType{};
    uint64_t sequence{};
    std::string json;
    uint32_t sourceOffset{};
};

using V4RowTypeMask = uint32_t;
constexpr V4RowTypeMask v4TypeBit(uint8_t type) { return type < 32 ? (1u << type) : 0; }
using IndexedCancelCheck = std::function<bool()>;

// Common read-only interface for indexed chunked generations. Each format keeps
// its own reader implementation while playback/export share the query surface.
class TnrdIndexedArchive {
public:
    virtual ~TnrdIndexedArchive() = default;
    virtual bool open(const std::string&, HeaderRow&, std::string*) = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;
    virtual const std::vector<V4LapInfo>& laps() const = 0;
    virtual const std::vector<V4ChunkInfo>& chunks() const = 0;
    virtual const V4ControlSummary& summary() const = 0;
    virtual float startTime() const = 0;
    virtual float totalTime() const = 0;
    virtual int lapAt(float) const = 0;
    virtual void chunkIndicesForLap(uint32_t, V4RowTypeMask, std::vector<size_t>&) const = 0;
    virtual bool chunkTimeBounds(size_t, float&, float&) const = 0;
    virtual void prefetchChunk(size_t) = 0;
    virtual void cancelPrefetch() = 0;
    virtual bool rowsForChunks(const std::vector<size_t>&,
        std::vector<std::vector<V4TimedRow>>&, std::string*) = 0;
    virtual bool rowsForLap(uint32_t, V4RowTypeMask, std::vector<V4TimedRow>&, std::string*) = 0;
    virtual bool rowsForLapRange(uint32_t, float, float, V4RowTypeMask,
        std::vector<V4TimedRow>&, std::string*, const IndexedCancelCheck& = {}) = 0;
    virtual bool rowsForRange(float, float, V4RowTypeMask, std::vector<V4TimedRow>&,
        std::string*, const IndexedCancelCheck& = {}) = 0;
    virtual bool latestRows(float, const std::vector<uint8_t>&, std::vector<V4TimedRow>&,
        std::string*, const IndexedCancelCheck& = {}) = 0;
    virtual bool forEachChunk(V4RowTypeMask,
        const std::function<bool(const V4ChunkInfo&, std::string_view)>&, std::string*) = 0;
    virtual void setCacheLimitBytes(size_t) = 0;
    virtual size_t cacheBytes() const = 0;
    virtual uint64_t decompressedChunkCount() const = 0;
    virtual size_t peakConcurrentChunkLoads() const = 0;
};

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
