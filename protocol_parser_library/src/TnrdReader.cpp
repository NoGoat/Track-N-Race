#include "tnrp/TnrdReader.h"
#include "tnrp/AnyRow.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <iterator>
#include <limits>
#include <system_error>

#include "TnrdCodec.h"
#include "tnrd/TnrdReaderBackend.h"
#include "tnrd/TNRD_V6.h"

namespace tnrp {

// Everything a recording's format decides is in its reader backend (one module
// per generation under tnrd/). This file only picks the backend and keeps what
// outlives a recording: host settings, the last error and analysis caches.

TnrdReader::TnrdReader() = default;
TnrdReader::~TnrdReader() { close(); }

bool TnrdReader::wasRecoveredV6() const { return backend_ && backend_->wasRecoveredV6(); }
bool TnrdReader::isLoaded() const { return backend_ && backend_->isLoaded(); }
float TnrdReader::startTime() const { return backend_ ? backend_->startTime() : 0.0f; }
float TnrdReader::totalTime() const { return backend_ ? backend_->totalTime() : 0.0f; }
int TnrdReader::effectivePlaybackDriver() const {
    return backend_ ? backend_->effectivePlaybackDriver() : -1;
}

void TnrdReader::sweepStaleTempFiles() noexcept {
    try {
        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path dir = fs::temp_directory_path(ec);
        if (ec) return;

        // Compare path::string_type directly (wstring on Windows, string on
        // POSIX). Converting every arbitrary temp entry with path::string()
        // can throw ERROR_NO_UNICODE_TRANSLATION before we even inspect it.
        const auto prefix = fs::path("tracknrace_").native();
        const auto suffix = fs::path(".tmp").native();

        int removed = 0;
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            const fs::path& p = it->path();
            const auto name = p.filename().native();
            // Match both apps' decompression temps: "tracknrace_*.tmp" (covers
            // current "tracknrace_temp_*" and legacy "tracknrace_<ts>.tmp").
            if (!name.starts_with(prefix) || !name.ends_with(suffix)) continue;
            std::error_code rmEc;
            if (fs::remove(p, rmEc)) ++removed;   // a held-open file simply fails; skip it
        }
        if (removed > 0)
            std::fprintf(stderr, "[tnrd] startup sweep: removed %d stale temp file(s)\n", removed);
    } catch (const std::exception& err) {
        // Startup cleanup is opportunistic. Report an unexpected filesystem
        // failure, but never let it escape through N-API or terminate Qt.
        std::fprintf(stderr, "[tnrd] startup sweep skipped: %s\n", err.what());
    } catch (...) {
        std::fprintf(stderr, "[tnrd] startup sweep skipped: unknown error\n");
    }
}

bool TnrdReader::load(const std::string& path, HeaderRow& outHeader) {
    context_.lastError.clear();
    std::string detectError;
    const TnrdFormat format = detail::detectTnrdFormat(path, &detectError);
    if (format == TnrdFormat::Unknown) {
        context_.lastError = detectError.empty() ? "The file format could not be identified." : detectError;
        close();
        std::fprintf(stderr, "[tnrd] load FAILED: %s for '%s'\n",
                     context_.lastError.c_str(), path.c_str());
        return false;
    }
    return loadWithFormat(path, outHeader, format);
}

bool TnrdReader::loadZstd(const std::string& path, HeaderRow& outHeader) {
    return loadWithFormat(path, outHeader, TnrdFormat::ZstdV2);
}

bool TnrdReader::loadGzip(const std::string& path, HeaderRow& outHeader) {
    return loadWithFormat(path, outHeader, TnrdFormat::GzipV1);
}

bool TnrdReader::loadWithFormat(const std::string& path, HeaderRow& outHeader,
                                TnrdFormat format) {
    close();
    context_.lastError.clear();
    std::string detectError;
    const TnrdFormat detected = detail::detectTnrdFormat(path, &detectError);
    const bool codecMatches = detected == format ||
        (isLegacyZstdStream(detected) && isLegacyZstdStream(format));
    if (!codecMatches) {
        context_.lastError = detected == TnrdFormat::Unknown
            ? (detectError.empty() ? "The file format could not be identified." : detectError)
            : std::string("Expected ") + toString(format) + " but found " + toString(detected) + ".";
        std::fprintf(stderr, "[tnrd] load FAILED: requested %s but file is %s ('%s')\n",
                     toString(format), toString(detected), path.c_str());
        return false;
    }
    std::fprintf(stderr, "[tnrd] load: '%s' (%s)\n", path.c_str(), toString(detected));
    std::unique_ptr<detail::TnrdReaderBackend> backend;
    switch (detected) {
        case TnrdFormat::GzipV1:    backend = detail::makeTnrdV1Reader(context_); break;
        case TnrdFormat::ZstdV2:    backend = detail::makeTnrdV2Reader(context_); break;
        case TnrdFormat::ZstdV3:    backend = detail::makeTnrdV3Reader(context_); break;
        case TnrdFormat::ChunkedV4: backend = detail::makeTnrdV4Reader(context_); break;
        case TnrdFormat::ChunkedV5: backend = detail::makeTnrdV5Reader(context_); break;
        case TnrdFormat::ChunkedV6: backend = detail::makeTnrdV6Reader(context_); break;
        default:
            context_.lastError = "The recording format has no reader implementation.";
            std::fprintf(stderr, "[tnrd] load FAILED: %s for '%s'\n",
                         context_.lastError.c_str(), path.c_str());
            return false;
    }
    if (!backend->load(path, outHeader)) {
        backend.reset();   // releases whatever the failed load opened
        close();
        return false;
    }
    backend_ = std::move(backend);
    loadedFormat_ = detected;
    return true;
}

bool TnrdReader::loadV6ArchiveForStrategy(std::unique_ptr<detail::TnrdV6Archive> archive,
                                          const HeaderRow& header) {
    close();
    context_.lastError.clear();
    if (!archive || !archive->isOpen()) return false;
    backend_ = detail::makeTnrdV6StrategyReader(context_, std::move(archive), header);
    loadedFormat_ = TnrdFormat::ChunkedV6;
    return true;
}

bool TnrdReader::summarize(const std::string& path, Summary& out, std::string* errorOut) {
    std::string error;
    const TnrdFormat format = detail::detectTnrdFormat(path, &error);
    bool ok = false;
    if (format == TnrdFormat::ChunkedV6) ok = detail::summarizeTnrdV6(path, out, error);
    else if (format == TnrdFormat::ChunkedV5) ok = detail::summarizeTnrdV5(path, out, error);
    else if (format == TnrdFormat::ChunkedV4) ok = detail::summarizeTnrdV4(path, out, error);
    else if (error.empty()) error = "This recording format has no index to read.";
    if (!ok && errorOut) *errorOut = error;
    return ok;
}

void TnrdReader::close() {
    ++loadSerial_;
    ersPeaks_.clear();
    if (ersPeakSweep_) ersPeakSweep_->cancel();
    ersPeakSweep_.reset();
    if (trackCornerSweep_) trackCornerSweep_->cancel();
    trackCornerSweep_.reset();
    trackCorners_.reset();
    std::fprintf(stderr, "[close-trace] TnrdReader::close entry format=%s\n", toString(loadedFormat_));
    std::fflush(stderr);
    backend_.reset();
    loadedFormat_ = TnrdFormat::Unknown;
    context_.playbackV6Types.clear();
    context_.playbackV6HistoryTypes.clear();
    context_.playbackRowMask = context_.playbackOutputRowMask;
    std::fprintf(stderr, "[close-trace] TnrdReader::close complete\n");
    std::fflush(stderr);
}

// ── Playback streaming ───────────────────────────────────────────────────────

void TnrdReader::setPlaybackRowMask(uint32_t mask, float cursorTime) {
    if (context_.playbackOutputRowMask == mask) return;
    context_.playbackOutputRowMask = mask;
    context_.playbackRowMask = mask;
    if (backend_) backend_->playbackRowMaskChanged(cursorTime);
}

void TnrdReader::setPlaybackV6Types(const std::vector<uint8_t>& streamTypes,
                                    const std::vector<uint8_t>& historyTypes,
                                    float cursorTime) {
    context_.playbackV6Types = streamTypes;
    context_.playbackV6HistoryTypes = historyTypes;
    if (backend_) backend_->playbackV6TypesChanged(cursorTime);
}

void TnrdReader::setPlaybackDriver(int driverIndex, bool useRecordedRows, float cursorTime) {
    if (backend_) backend_->setPlaybackDriver(driverIndex, useRecordedRows, cursorTime);
}

void TnrdReader::setV6FocusDriver(int driverIndex, float cursorTime) {
    if (backend_) backend_->setV6FocusDriver(driverIndex, cursorTime);
}

std::vector<std::string> TnrdReader::v6FocusStatusSnapshot(float t) {
    return backend_ ? backend_->v6FocusStatusSnapshot(t) : std::vector<std::string>{};
}

void TnrdReader::setCursor(float t) { if (backend_) backend_->setCursor(t); }
void TnrdReader::beginCursorPrime(float t) { if (backend_) backend_->beginCursorPrime(t); }
void TnrdReader::primeCursor() { if (backend_) backend_->primeCursor(); }

std::vector<std::string> TnrdReader::pullUntil(float t) {
    return backend_ ? backend_->pullUntil(t) : std::vector<std::string>{};
}

std::vector<std::string> TnrdReader::drainRest() {
    // +INFINITY compares greater than every finite sessionTime, so pullUntil
    // walks to the end of the recording.
    return pullUntil(INFINITY);
}

bool TnrdReader::hasMore() const { return backend_ && backend_->hasMore(); }

void TnrdReader::pullUntilSplit(float t, std::string& jsonOut, std::vector<uint8_t>& binOut,
                                uint32_t& seenTypes,
                                std::array<std::string, 16>* lastOfType) {
    if (backend_) backend_->pullUntilSplit(t, jsonOut, binOut, seenTypes, lastOfType);
}

// ── Seek support ─────────────────────────────────────────────────────────────

std::vector<std::pair<uint8_t, std::string>> TnrdReader::latestOfTypesTagged(
    float t, const std::vector<uint8_t>& types,
    const std::function<bool()>& cancelled) {
    if (!backend_) return {};
    return backend_->latestOfTypesTagged(t, types, cancelled);
}

std::vector<std::string> TnrdReader::latestOfTypes(float t, const std::vector<uint8_t>& types,
                                                   const std::function<bool()>& cancelled) {
    std::vector<std::string> out;
    for (auto& [tid, line] : latestOfTypesTagged(t, types, cancelled)) {
        (void)tid;
        out.push_back(std::move(line));
    }
    return out;
}

std::vector<std::string> TnrdReader::stateSnapshot(float t, const std::function<bool()>& cancelled) {
    // Panel-state setters, emitted individually: session, lap, timing,
    // participants, all_status, tyre_sets.
    static constexpr uint8_t STATE_TYPE_IDS[] = { 4, 5, 7, 8, 9, 10 };
    return latestOfTypes(t, { std::begin(STATE_TYPE_IDS), std::end(STATE_TYPE_IDS) }, cancelled);
}

std::vector<std::string> TnrdReader::readRange(float fromTime, float toTime) {
    return backend_ ? backend_->readRange(fromTime, toTime) : std::vector<std::string>{};
}

void TnrdReader::setStrategyMinimumStops(int stops) {
    const int clamped = std::clamp(stops, 0, 8);
    if (context_.strategyMinimumStops == clamped) return;
    context_.strategyMinimumStops = clamped;
    if (backend_) backend_->clearStrategyCheckpoints();
}

void TnrdReader::setTeamColorOverrides(TeamColorOverrides overrides) {
    TeamColorOverrides sanitized = sanitizeTeamColorOverrides(overrides);
    if (context_.teamColorOverrides == sanitized) return;
    context_.teamColorOverrides = std::move(sanitized);
    if (backend_) backend_->clearStrategyCheckpoints();
}

StrategySnapshotRow TnrdReader::strategySnapshotAt(float t, StrategyProcessor* restoredProcessor,
                                                   const std::function<bool()>& cancelled) {
    if (backend_) return backend_->strategySnapshotAt(t, restoredProcessor, cancelled);
    StrategyProcessor processor(2025);
    processor.setMinimumStops(context_.strategyMinimumStops);
    processor.setTeamColorOverrides(context_.teamColorOverrides);
    StrategySnapshotRow result = processor.snapshot();
    if (restoredProcessor) *restoredProcessor = std::move(processor);
    return result;
}

std::vector<std::string> TnrdReader::damageRowsAtCadence(
    float fromTime, float toTime, bool includeFrom,
    const std::function<bool()>& cancelled) const {
    if (!backend_) return {};
    return backend_->damageRowsAtCadence(fromTime, toTime, includeFrom, cancelled);
}

bool TnrdReader::currentLapAt(float t, float& startOut, int& numOut) const {
    return backend_ && backend_->currentLapAt(t, startOut, numOut);
}

TnrdReader::SeekFlush TnrdReader::seekFlush(float target, float currentLapStart,
                                            bool allHistory, uint32_t requestedTypes,
                                            float windowSeconds, bool includeMandatoryState,
                                            const std::function<bool()>& cancelled) {
    if (!backend_) return {};
    return backend_->seekFlush(target, currentLapStart, allHistory, requestedTypes,
                               windowSeconds, includeMandatoryState, cancelled);
}

// ── Chart history on a second handle ─────────────────────────────────────────

std::unique_ptr<TnrdReader::HistoryReader> TnrdReader::openHistoryReader(std::string* errorOut) const {
    return backend_ ? backend_->openHistoryReader(errorOut) : nullptr;
}

bool TnrdReader::historyRequest(float from, float to, uint32_t mask, HistoryRequest& out) const {
    return backend_ && backend_->historyRequest(from, to, mask, out);
}

// ── Load-time payload ────────────────────────────────────────────────────────

std::string TnrdReader::lapBlocksMessage() const {
    if (backend_) return backend_->lapBlocksMessage();
    return writeJson(PlaybackLapBlocksRow{});
}

std::string TnrdReader::driverRestrictionMessage(float cursorTime) const {
    return backend_ ? backend_->driverRestrictionMessage(cursorTime) : std::string{};
}

std::string TnrdReader::getLapDataMessage(int lapNum, uint32_t rowTypeMask, int driverIndex) const {
    return backend_ ? backend_->getLapDataMessage(lapNum, rowTypeMask, driverIndex) : std::string{};
}

std::string TnrdReader::phoneChartLapJson(int lapNum, const std::vector<std::string>& channels) const {
    return backend_ ? backend_->phoneChartLapJson(lapNum, channels) : std::string{};
}

// ── Analysis ─────────────────────────────────────────────────────────────────

bool TnrdReader::getAnalysisLapProgress(int lapNum, AnalysisLapProgress& out, int driverIndex) const {
    return backend_ && backend_->getAnalysisLapProgress(lapNum, out, driverIndex);
}

std::vector<int> TnrdReader::analysisLapNumbers(int driverIndex) const {
    return backend_ ? backend_->analysisLapNumbers(driverIndex) : std::vector<int>{};
}

bool TnrdReader::getAnalysisErsSamples(int lapNum, std::vector<ErsSample>& out, int driverIndex) const {
    return backend_ && backend_->getAnalysisErsSamples(lapNum, out, driverIndex);
}

bool TnrdReader::getAnalysisHarvestSamples(int lapNum, std::vector<HarvestSample>& out,
                                           int driverIndex) const {
    return backend_ && backend_->getAnalysisHarvestSamples(lapNum, out, driverIndex);
}

bool TnrdReader::getAnalysisInputSamples(int lapNum, std::vector<InputSample>& out,
                                         int driverIndex) const {
    return backend_ && backend_->getAnalysisInputSamples(lapNum, out, driverIndex);
}

bool TnrdReader::getAnalysisPositionSamples(int lapNum, std::vector<PlayerPositionPoint>& out,
                                            int driverIndex) const {
    return backend_ && backend_->getAnalysisPositionSamples(lapNum, out, driverIndex);
}

std::vector<float> TnrdReader::trackLimitTimes(int driverIndex) const {
    std::vector<float> times;
    if (!backend_) return times;
    // Infringement types for corner cutting and running wide; the same codes
    // in the F1 24, F1 25 and 2026 specifications.
    const auto isTrackLimit = [](int type) {
        return (type >= 7 && type <= 9) || (type >= 25 && type <= 29);
    };
    const int car = analysisCarIndex(driverIndex);
    for (const std::string& line : backend_->raceEventLines()) {
        const std::optional<AnyRow> row = parseRow(line);
        const RaceEventRow* event = row ? std::get_if<RaceEventRow>(&*row) : nullptr;
        if (event && event->code == "PENA" && event->car_idx == car &&
            event->infringement_type && isTrackLimit(*event->infringement_type))
            times.push_back(event->session_time);
    }
    return times;
}

std::vector<TrackStatusChange> TnrdReader::aeroTrackStatusTimeline() const {
    return backend_ ? backend_->aeroTrackStatusTimeline() : std::vector<TrackStatusChange>{};
}

DriverLapHistoryRow TnrdReader::driverLapHistory(int driver, float throughTime) const {
    if (backend_) return backend_->driverLapHistory(driver, throughTime);
    DriverLapHistoryRow out;
    out.car_idx = driver;
    return out;
}

LapMeasuresRow TnrdReader::lapMeasures(int driver, float throughTime, bool hasMguh) const {
    if (backend_) return backend_->lapMeasures(driver, throughTime, hasMguh);
    LapMeasuresRow out;
    out.car_idx = driver;
    return out;
}

bool TnrdReader::v6FastestLapAt(float t, FastestLapRow& out) const {
    return backend_ && backend_->v6FastestLapAt(t, out);
}

float TnrdReader::v6NextLapEndAfter(float t) const {
    return backend_ ? backend_->v6NextLapEndAfter(t) : std::numeric_limits<float>::infinity();
}

bool TnrdReader::cachedTrackCorners(TrackCorners& out) const {
    if (!trackCorners_) return false;
    out = *trackCorners_;
    return true;
}

void TnrdReader::cacheTrackCorners(uint64_t serial, const TrackCorners& corners) const {
    if (serial == loadSerial_) trackCorners_ = corners;
}

bool TnrdReader::cachedErsPeaks(int driverIndex, ErsPeaks& out) const {
    const auto it = ersPeaks_.find(analysisCarIndex(driverIndex));
    if (it == ersPeaks_.end()) return false;
    out = it->second;
    return true;
}

void TnrdReader::cacheErsPeaks(int driverIndex, uint64_t serial, const ErsPeaks& peaks) const {
    if (serial == loadSerial_) ersPeaks_[analysisCarIndex(driverIndex)] = peaks;
}

void TnrdReader::startErsPeakSweep() {
    if (ersPeakSweep_ || !backend_) return;
    ersPeakSweep_ = backend_->startErsPeakSweep();
}

void TnrdReader::startTrackCornerSweep() {
    if (trackCornerSweep_ || !backend_) return;
    trackCornerSweep_ = backend_->startTrackCornerSweep();
}

} // namespace tnrp
