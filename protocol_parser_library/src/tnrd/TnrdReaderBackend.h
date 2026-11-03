#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "tnrp/TnrdReader.h"

namespace tnrp::detail {

// The interface TnrdReader forwards to: one loaded recording, read the way its
// TNRD generation stores it. Each generation implements all of it in its own
// module (TNRD_V1_Reader.cpp ... TNRD_V6_Reader.cpp) and shares no code with
// the others; this header only declares the calls.
//
// Host settings and the last error live in the TnrdReaderContext the reader
// owns, which each implementation receives when it is made.
class TnrdReaderBackend {
public:
    using SeekFlush = TnrdReader::SeekFlush;
    using CancelCheck = std::function<bool()>;

    virtual ~TnrdReaderBackend() = default;

    virtual TnrdFormat format() const = 0;
    virtual float startTime() const = 0;
    virtual float totalTime() const = 0;

    // Opens the recording at `path`. On failure lastError says why and the
    // backend is discarded.
    virtual bool load(const std::string& path, HeaderRow& outHeader) = 0;
    virtual bool isLoaded() const = 0;

    // ── Playback streaming ──────────────────────────────────────────────────
    virtual bool hasMore() const = 0;
    virtual void setCursor(float t) = 0;
    virtual void beginCursorPrime(float t) = 0;
    virtual void primeCursor() = 0;
    virtual std::vector<std::string> pullUntil(float t) = 0;
    virtual void pullUntilSplit(float t, std::string& jsonOut, std::vector<uint8_t>& binOut,
                                uint32_t& seenTypes, std::array<std::string, 16>* lastOfType) = 0;

    // ── Seek support ────────────────────────────────────────────────────────
    virtual std::vector<std::pair<uint8_t, std::string>> latestOfTypesTagged(
        float t, const std::vector<uint8_t>& types, const CancelCheck& cancelled) = 0;
    virtual std::vector<std::string> readRange(float fromTime, float toTime) = 0;
    virtual std::vector<std::string> damageRowsAtCadence(
        float fromTime, float toTime, bool includeFrom, const CancelCheck& cancelled) const = 0;
    virtual bool currentLapAt(float t, float& startOut, int& numOut) const = 0;
    virtual SeekFlush seekFlush(float target, float currentLapStart, bool allHistory,
                                uint32_t requestedTypes, float windowSeconds,
                                bool includeMandatoryState, const CancelCheck& cancelled) = 0;
    virtual StrategySnapshotRow strategySnapshotAt(float t, StrategyProcessor* restoredProcessor,
                                                   const CancelCheck& cancelled) = 0;
    // Strategy settings changed: cached checkpoints no longer apply.
    virtual void clearStrategyCheckpoints() = 0;

    // ── Chart history on a second handle (TnrdReader::HistoryReader) ────────
    virtual std::unique_ptr<TnrdReader::HistoryReader> openHistoryReader(std::string* errorOut) const = 0;
    virtual bool historyRequest(float from, float to, uint32_t mask,
                                TnrdReader::HistoryRequest& out) const = 0;

    // ── Host playback settings, already stored in the context ───────────────
    virtual void playbackRowMaskChanged(float cursorTime) = 0;
    virtual void playbackV6TypesChanged(float cursorTime) = 0;
    virtual void setPlaybackDriver(int driverIndex, bool useRecordedRows, float cursorTime) = 0;
    virtual void setV6FocusDriver(int driverIndex, float cursorTime) = 0;
    virtual std::vector<std::string> v6FocusStatusSnapshot(float t) = 0;
    virtual int effectivePlaybackDriver() const = 0;
    virtual bool wasRecoveredV6() const = 0;

    // ── Load-time payload ───────────────────────────────────────────────────
    virtual std::string lapBlocksMessage() const = 0;
    virtual std::string driverRestrictionMessage(float cursorTime) const = 0;
    virtual std::string getLapDataMessage(int lapNum, uint32_t rowTypeMask, int driverIndex) const = 0;
    virtual std::string phoneChartLapJson(int lapNum, const std::vector<std::string>& channels) const = 0;

    // ── Analysis ────────────────────────────────────────────────────────────
    virtual bool getAnalysisLapProgress(int lapNum, AnalysisLapProgress& out, int driverIndex) const = 0;
    virtual std::vector<int> analysisLapNumbers(int driverIndex) const = 0;
    virtual bool getAnalysisErsSamples(int lapNum, std::vector<ErsSample>& out, int driverIndex) const = 0;
    virtual bool getAnalysisHarvestSamples(int lapNum, std::vector<HarvestSample>& out,
                                           int driverIndex) const = 0;
    virtual bool getAnalysisInputSamples(int lapNum, std::vector<InputSample>& out,
                                         int driverIndex) const = 0;
    virtual bool getAnalysisPositionSamples(int lapNum, std::vector<PlayerPositionPoint>& out,
                                            int driverIndex) const = 0;
    virtual std::vector<TrackStatusChange> aeroTrackStatusTimeline() const = 0;
    virtual std::shared_ptr<ErsPeakSweep> startErsPeakSweep() = 0;
    virtual std::shared_ptr<TrackCornerSweep> startTrackCornerSweep() = 0;
    virtual DriverLapHistoryRow driverLapHistory(int driver, float throughTime) const = 0;
    virtual LapMeasuresRow lapMeasures(int driver, float throughTime, bool hasMguh) const = 0;
    virtual bool v6FastestLapAt(float t, FastestLapRow& out) const = 0;
    virtual float v6NextLapEndAfter(float t) const = 0;

    // ── XLSX export ─────────────────────────────────────────────────────────
    // Where a V6 row came from, for the V6 sheet layout's extra columns.
    struct ExportChunkMeta {
        int driverIndex{};
        uint32_t lapId{};
        uint32_t lapNumber{};
        bool formation{};
    };
    // Called once per stored row: its JSONL text and session time (NaN when the
    // caller should read it from the row). Returning false means later rows of
    // this type are only counted, so they may arrive with an empty line.
    using ExportRowFn = std::function<bool(std::string_view line, float sessionTime,
                                           const ExportChunkMeta* meta)>;
    virtual bool exportUsesV6Layout() const = 0;
    // The row types with a sheet (`hasSheet`), in first-stored order, and how
    // many rows the export will visit.
    virtual void exportPlan(const std::function<bool(uint8_t)>& hasSheet,
                            std::vector<uint8_t>& typeOrder, size_t& total) const = 0;
    virtual bool exportRows(uint8_t type, const ExportRowFn& row, std::string* errorOut) = 0;
};

// Each generation's reader, from its own module.
std::unique_ptr<TnrdReaderBackend> makeTnrdV1Reader(TnrdReaderContext& context);
std::unique_ptr<TnrdReaderBackend> makeTnrdV2Reader(TnrdReaderContext& context);
std::unique_ptr<TnrdReaderBackend> makeTnrdV3Reader(TnrdReaderContext& context);
std::unique_ptr<TnrdReaderBackend> makeTnrdV4Reader(TnrdReaderContext& context);
std::unique_ptr<TnrdReaderBackend> makeTnrdV5Reader(TnrdReaderContext& context);
std::unique_ptr<TnrdReaderBackend> makeTnrdV6Reader(TnrdReaderContext& context);
// A V6 reader over an archive that is already open, for Strategy only.
std::unique_ptr<TnrdReaderBackend> makeTnrdV6StrategyReader(
    TnrdReaderContext& context, std::unique_ptr<TnrdV6Archive> archive, const HeaderRow& header);

// TnrdReader::summarize() for the generations whose index says it.
bool summarizeTnrdV4(const std::string& path, TnrdReader::Summary& out, std::string& error);
bool summarizeTnrdV5(const std::string& path, TnrdReader::Summary& out, std::string& error);
bool summarizeTnrdV6(const std::string& path, TnrdReader::Summary& out, std::string& error);

} // namespace tnrp::detail
