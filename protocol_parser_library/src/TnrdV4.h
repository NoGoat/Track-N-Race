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
};

using V4RowTypeMask = uint32_t;
constexpr V4RowTypeMask v4TypeBit(uint8_t type) { return type < 32 ? (1u << type) : 0; }

// Direct indexed V4 reader. open() reads and validates only the uncompressed
// control plane. Chunk payloads are decompressed on demand into a bounded LRU.
class TnrdV4Archive {
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

    bool rowsForLap(uint32_t lap, V4RowTypeMask mask,
                    std::vector<V4TimedRow>& out, std::string* errorOut);
    bool rowsForRange(float fromTime, float toTime, V4RowTypeMask mask,
                      std::vector<V4TimedRow>& out, std::string* errorOut);
    bool latestRows(float atTime, const std::vector<uint8_t>& types,
                    std::vector<V4TimedRow>& out, std::string* errorOut);
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

} // namespace tnrp::detail
