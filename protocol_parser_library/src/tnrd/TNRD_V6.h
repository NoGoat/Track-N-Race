#pragma once

#include "TNRD_V4.h"

#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tnrp::detail {

enum class V6DataType : uint8_t {
    Unknown = 0, Speed = 1, RPM = 2, Gear = 3, Throttle = 4, Brake = 5,
    Steering = 6, Aero = 7, TyreSurfaceTemp = 8, TyreInnerTemp = 9,
    BrakeTemp = 10, EngineTemp = 11, TyreWear = 12, TyreState = 13,
    Damage = 14, Fuel = 15, ERSStore = 16, ERSHarvest = 17,
    ERSDeployment = 18, EnginePower = 19, BrakeBias = 20, GForce = 21,
    RideHeight = 22, Position = 23, LapTiming = 24, Count = 25
};

constexpr uint64_t v6DataTypeBit(V6DataType type) {
    return static_cast<uint8_t>(type) < 64
        ? (uint64_t{1} << static_cast<uint8_t>(type)) : 0;
}
const char* v6TypeName(V6DataType type);
V6DataType v6TypeFromName(std::string_view name);
// True for the edge-encoded types (Aero, TyreState, BrakeBias): the writer only
// records a sample when the value changes, so the newest sample at or before a
// cursor can be arbitrarily far behind it. Consumers restoring state at a cursor
// have to account for that; see readMultiDriverLatest().
bool stateType(V6DataType type);

enum class TelemetrySetting : uint8_t { Unknown = 0, Restricted = 1, Public = 2 };
enum class V6Phase : uint8_t { Race = 0, Formation = 1 };

struct V6RestrictionChange {
    V6Phase phase{V6Phase::Race};
    float sessionTime{};
    TelemetrySetting setting{TelemetrySetting::Unknown};
};

struct V6TyreStintSummary {
    int endLap{};
    int actualCompound{};
    int visualCompound{};
};

struct V6DriverHeader {
    uint8_t vehicleIndex{};
    std::string driverName;
    int teamId{-1};
    int raceNumber{-1};
    bool isPlayer{};
    TelemetrySetting initialTelemetrySetting{TelemetrySetting::Unknown};
    std::vector<V6RestrictionChange> restrictionChanges;
    uint64_t availableTypeMask{};
    std::vector<uint32_t> lapIds;
    std::vector<V6TyreStintSummary> tyreStints;
};

struct V6LapSummary {
    uint32_t lapId{};
    uint8_t driverIndex{};
    uint32_t lapNumber{};
    V6Phase phase{V6Phase::Race};
    float startSessionTime{};
    float endSessionTime{};
    uint32_t lapTimeMs{};
    uint32_t s1Ms{};
    uint32_t s2Ms{};
    uint32_t s3Ms{};
    bool isCompleted{};
    bool isValid{true};
    bool isPartial{};
};

struct V6ChunkInfo {
    uint8_t driverIndex{};
    uint32_t lapId{};
    uint8_t typeId{};
    uint8_t flags{};
    V6Phase phase{V6Phase::Race};
    float firstTime{};
    float lastTime{};
    uint64_t offset{};
    uint64_t compressedSize{};
    uint64_t uncompressedSize{};
    uint32_t sampleCount{};
    uint32_t checksum{};
    uint64_t sequence{};
};

struct V6SharedRecord {
    V6Phase phase{V6Phase::Race};
    float sessionTime{};
    std::string json;
};

using V6SourceRow = V4SourceRow;
using V6LapInfo = V4LapInfo;
using V6LapStatusSummary = V4LapStatusSummary;
using V6ControlSummary = V4ControlSummary;
using V6TimedRow = V4TimedRow;
using V6RowTypeMask = V4RowTypeMask;

// A live session held in memory by a TnrdV6Writer (openMemory) at one moment,
// laid out as a V6 file's index would describe it. Each chunk's
// V6ChunkInfo::offset indexes `payloads`: a committed chunk's zstd frame,
// shared with the writer, or a lap still being built encoded for this image
// alone. Immutable once built, so it can be read on any thread.
struct V6MemoryChunk {
    std::shared_ptr<const std::vector<uint8_t>> payload;
    bool compressed{};
};
struct V6MemoryImage {
    HeaderRow session;
    // The phase being recorded; reads expose it instead of the race when the
    // session is still on its formation lap.
    V6Phase phase{V6Phase::Race};
    std::vector<V6DriverHeader> drivers;
    std::vector<V6LapSummary> laps;
    std::vector<V6ChunkInfo> chunks;   // lap clocks included
    std::vector<V6MemoryChunk> payloads;
    std::vector<V6SharedRecord> shared;
    size_t encodedBytes{};             // open laps encoded for this image
};
// What a memory image carries. Lap summaries and driver headers are always
// complete; chunks only for the drivers and types asked for, plus lap clocks.
struct V6ImageFilter {
    bool chunks{true};
    bool playerOnly{};
    std::vector<uint8_t> drivers;      // empty: every driver
    std::vector<uint8_t> types;        // empty: every type
    bool shared{};
    uint8_t sharedType{};              // legacy row type (5, 6, 8); 0: all
    // Finite: only the recorded phase's records from this time on.
    float sharedFrom{-std::numeric_limits<float>::infinity()};
};

struct TnrdV6WriterMemoryStats {
    bool open{};
    size_t retainedBytes{}, builderCount{}, builderPlainBytes{}, builderPlainCapacityBytes{};
    size_t builderRowIndexEntries{}, builderRowIndexCapacityBytes{};
    size_t pendingLapCount{}, pendingLapPlainBytes{};
    size_t chunkCount{}, chunkContainerCapacityBytes{}, chunkRowIndexEntries{};
    size_t chunkRowIndexCapacityBytes{}, branchCount{}, branchCapacityBytes{};
    size_t lapCount{}, statusLapCount{}, eventCount{}, eventPayloadBytes{};
    size_t eventPayloadCapacityBytes{}, eventContainerCapacityBytes{}, lapStatusCapacityBytes{};
    uint64_t chunkWrites{}, chunkPlainBytesProcessed{}, chunkCompressedBytesWritten{};
    uint64_t compressionBufferBytesAllocated{};
    size_t compressionScratchCapacityBytes{}, compressionContextBytes{};
    size_t lastChunkPlainBytes{}, lastChunkCompressedBytes{};
    size_t lastCompressionBufferCapacityBytes{}, peakCompressionBufferCapacityBytes{};
    uint64_t checkpointWrites{}, checkpointScratchBytesAllocated{};
    size_t lastCheckpointScratchBytes{}, peakCheckpointScratchBytes{};
    size_t lastCheckpointDirectoryBytes{}, peakCheckpointDirectoryBytes{};
    size_t lastCheckpointRowIndexBytes{}, peakCheckpointRowIndexBytes{};
    // openMemory() only: committed chunk frames and shared records held.
    size_t memoryChunkBytes{}, memorySharedRecords{}, memorySharedBytes{};
    uint64_t uncommittedLaps{};
};

// A decoded chunk whose rows are rendered one at a time, when asked for.
// Playback holds these instead of rendering every driver's whole lap the
// moment a seek lands in it. Holding one keeps its decoded chunk alive.
class V6RowSource {
public:
    virtual ~V6RowSource() = default;
    // The row exactly as rowsForChunks() renders it.
    virtual std::string render(uint32_t row) const = 0;
};
struct V6DeferredRow {
    float sessionTime{};
    uint32_t row{};  // handle for V6RowSource::render
};

class TnrdV6Archive final : public TnrdIndexedArchive {
public:
    TnrdV6Archive();
    ~TnrdV6Archive();
    TnrdV6Archive(const TnrdV6Archive&) = delete;
    TnrdV6Archive& operator=(const TnrdV6Archive&) = delete;

    bool open(const std::string&, HeaderRow&, std::string*) override;
    // Reads a live session's memory image as open() reads a file. The image
    // stays alive with the archive.
    bool openMemory(std::shared_ptr<const V6MemoryImage>, HeaderRow&, std::string*);
    void close() override;
    bool isOpen() const override;
    const std::vector<V6LapInfo>& laps() const override;
    const std::vector<V4ChunkInfo>& chunks() const override;
    const V6ControlSummary& summary() const override;
    float startTime() const override;
    float totalTime() const override;
    int lapAt(float) const override;
    void chunkIndicesForLap(uint32_t, V6RowTypeMask, std::vector<size_t>&) const override;
    bool chunkTimeBounds(size_t, float&, float&) const override;
    void prefetchChunk(size_t) override;
    void cancelPrefetch() override;
    bool rowsForChunks(const std::vector<size_t>&, std::vector<std::vector<V6TimedRow>>&,
                       std::string*) override;
    // rowsForChunks() for one chunk without rendering anything: the rows it
    // would return that are later than `after`, in the same order, plus the
    // newest finite time among all of them (-infinity when there is none).
    bool deferredRowsForChunk(size_t index, float after,
                              std::shared_ptr<const V6RowSource>& source,
                              std::vector<V6DeferredRow>& rows, float& maxTime,
                              std::string* errorOut);
    bool rowsForLap(uint32_t, V6RowTypeMask, std::vector<V6TimedRow>&, std::string*) override;
    bool rowsForLapRange(uint32_t, float, float, V6RowTypeMask,
                         std::vector<V6TimedRow>&, std::string*,
                         const IndexedCancelCheck& = {}) override;
    bool rowsForRange(float, float, V6RowTypeMask, std::vector<V6TimedRow>&,
                      std::string*, const IndexedCancelCheck& = {}) override;
    bool forEachRowInRange(float, float, V6RowTypeMask,
                           const std::function<bool(const V6TimedRow&)>&,
                           std::string*, const IndexedCancelCheck& = {}) override;
    bool latestRows(float, const std::vector<uint8_t>&, std::vector<V6TimedRow>&,
                    std::string*, const IndexedCancelCheck& = {}) override;
    bool forEachChunk(V6RowTypeMask,
                      const std::function<bool(const V4ChunkInfo&, std::string_view)>&,
                      std::string*) override;
    void setCacheLimitBytes(size_t) override;
    size_t cacheBytes() const override;
    uint64_t decompressedChunkCount() const override;
    size_t peakConcurrentChunkLoads() const override;

    void setPlaybackDriver(uint8_t);
    // A second car whose private status families (ERS, fuel, aero, brake bias,
    // engine power) are streamed alongside the playback driver's, for a
    // Standings row the user has selected. Every other car contributes only
    // its public all-car families. -1 clears it.
    void setFocusDriver(int);
    int focusDriver() const;
    void setRequestedTypes(const std::vector<uint8_t>&);
    bool requestedType(uint8_t) const;
    void playbackChunkIndices(V6RowTypeMask, std::vector<size_t>&) const;
    uint8_t playbackDriver() const;
    std::optional<uint8_t> playerDriverIndex() const;
    const std::vector<V6DriverHeader>& driverHeaders() const;
    const V6DriverHeader* driverHeader(uint8_t) const;
    // That driver's "Your Telemetry" setting as of a logical playback time,
    // i.e. the header's initial setting with every change at or before it
    // applied. Unknown until the first Participants update that carried one.
    TelemetrySetting telemetrySettingAt(uint8_t, float) const;
    // Whether the driver's private data is readable at that time: the recording
    // player's own car always is, regardless of what they broadcast.
    bool privateDataAvailableAt(uint8_t, float) const;
    std::vector<V6LapSummary> driverLapSummaries(uint8_t) const;
    const std::vector<V6ChunkInfo>& v6Chunks() const;
    const std::vector<V6SharedRecord>& sharedRecords() const;
    float logicalTime(V6Phase, float) const;
    bool loadChunkPlain(size_t, std::shared_ptr<std::string>&, std::string*);
    bool readDriverLapTypes(uint8_t, uint32_t, const std::vector<uint8_t>&,
                            std::vector<V6TimedRow>&, std::string*);
    bool readDriverRangeTypes(uint8_t, float, float, const std::vector<uint8_t>&,
                              std::vector<V6TimedRow>&, std::string*,
                              const IndexedCancelCheck& = {});
    bool readMultiDriverLatest(float, const std::vector<uint8_t>&,
                               const std::vector<uint8_t>&,
                               std::vector<V6TimedRow>&, std::string*);
    // One driver's race-phase samples in [from, to] as typed column blocks,
    // one block per V6 type, with no JSON anywhere on the path. `types` empty
    // means every type; `v4Mask` keeps only types feeding those legacy row
    // families. With `seed`, each type first carries its last value(s) at or
    // before `from`, stamped at `from`, so a range that starts between samples
    // still knows the current state. The layout is documented at
    // V6_HISTORY_MAGIC in TNRD_V6.cpp and decoded by the Electron renderer's
    // lib/columnStore.ts. Safe to call concurrently with another archive
    // instance; not with a second call on this one.
    bool columnarHistory(uint8_t driver, const std::vector<uint8_t>& types, uint32_t v4Mask,
                         float from, float to, bool seed, std::vector<uint8_t>& out,
                         std::string* errorOut, const IndexedCancelCheck& cancelled = {});
    // True when this recording was opened by rebuilding its index from the
    // chunk stream, because the writer was interrupted before writing one.
    // Callers should surface this: the final lap of each driver may be absent.
    bool wasRecovered() const;

private:
    bool recoverByScan(HeaderRow&, std::string*);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class TnrdV6Writer {
public:
    TnrdV6Writer();
    ~TnrdV6Writer();
    TnrdV6Writer(const TnrdV6Writer&) = delete;
    TnrdV6Writer& operator=(const TnrdV6Writer&) = delete;
    bool open(const std::string&, const HeaderRow&, std::string*);
    // A live session's store: the laps, chunks, encoding and write delay of a
    // file, kept in memory (see TnrdWriter's retained session). Unlike a file,
    // a rewind may reach committed laps; they are decoded and reopened rather
    // than refused.
    bool openMemory(const HeaderRow&, std::string*);
    // openMemory() only: the session as it stands, for TnrdV6Archive::openMemory.
    std::shared_ptr<const V6MemoryImage> memoryImage(const V6ImageFilter&) const;
    void setProtocol(int);
    // openMemory() only: records the session to a file as well. The file
    // starts with everything committed so far, then takes each commit as it
    // happens. detachFile() writes the laps still being built and the index,
    // as finish() would, and closes the file; the session carries on in
    // memory. A rewind into committed laps rewrites the file from memory.
    // Up to two files can be attached at once. A `playerOnly` file takes only
    // the player's chunks, laps and driver header; the shared records (session,
    // participants, events) go to every file.
    bool attachFile(const std::string& path, const HeaderRow&, std::string*, bool playerOnly = false);
    // Finishes every attached file.
    bool detachFile(std::string*);
    bool hasFile() const;
    // openMemory() only: each attached file closed where a write failed, as
    // (path, why), once; empty otherwise. The session itself is unaffected.
    std::vector<std::pair<std::string, std::string>> takeFileErrors();
    bool append(const std::vector<V6SourceRow>&, std::string*);
    bool appendViews(const std::vector<std::pair<std::string_view, float>>&, std::string*);
    bool appendRow(std::string_view, float, std::string*);
    bool advanceSessionTime(float, std::string*);
    bool checkpoint(std::string*);
    // Zstandard level for chunks and shared records. Out-of-range values fall
    // back to the default of 9; see docs/TNRD_V6_WRITER_EFFICIENCY_DESIGN.md.
    void setCompressionLevel(int);
    int compressionLevel() const;
    // Car Telemetry 2 m_2026Regulations; stored in the session header when the
    // file is finished (the opening session record cannot know it yet).
    void setRegulations2026(bool);
    bool rewind(float, std::string*);
    void abort();
    bool finish(std::string*);
    bool isOpen() const;
    TnrdV6WriterMemoryStats memoryStats() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

bool writeTnrdV6(const std::string&, const HeaderRow&,
                 const std::vector<V6SourceRow>&, std::string*);
struct V6LoadResult { HeaderRow header; std::unique_ptr<TnrdV6Archive> archive; };
namespace TNRD_V6 { bool load(const std::string&, V6LoadResult&, std::string&); }

} // namespace tnrp::detail
