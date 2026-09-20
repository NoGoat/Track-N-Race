#include "tnrp/Engine.h"
#include "tnrp/BinaryRows.h"
#include "tnrp/LapDelta.h"
#include "tnrp/TimeUtils.h"
#include "tnrp/control_rows.h"
#include "LiveHistoryStore.h"
#include "StrategyRollback.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <set>
#include <string_view>
#include <thread>
#include <utility>

#define TRACE(msg) do { fprintf(stderr, "[native] " msg "\n"); fflush(stderr); } while (0)

namespace tnrp {

// Sparse panel-row type ids re-emitted each binary-playback tick so their
// panels track the playhead between native ~2 Hz updates. Damage (id 3) is
// excluded: TnrdReader reconstructs it independently at its specified 10 Hz.
// Lap rows (id 4) are measurements, not panel-only state. Re-emitting one with
// the playhead's session_time while retaining its recorded current_lap_ms and
// lap_distance_m creates a false timing sample and corrupts distance-based
// delta calculations. Only original recorded lap rows may advance lap data.
static constexpr uint8_t kDupTypeIds[] = { 2, 5, 7, 9, 13 };
static constexpr uint32_t kDupRowMask =
    (1u << 2) | (1u << 5) | (1u << 7) | (1u << 9) | (1u << 13);
// Seek history contains sampled/hot rows, not the rare Participants or tyre-set
// state. These families must always be reconstructed at the target even when
// the renderer's requested row mask includes them; otherwise paired clients
// have no current roster/set snapshot to receive or request after a seek.
static constexpr uint32_t kSeekPanelRestoreMask =
    kDupRowMask | (1u << 8) | (1u << 10);
static constexpr uint32_t kRestoreRowMask =
    (1u << 2) | (1u << 3) | (1u << 4) | (1u << 5) | (1u << 7) |
    (1u << 8) | (1u << 9) | (1u << 10) | (1u << 13) | (1u << 14);
static constexpr uint32_t kHistoricalRowMask =
    (1u << 1) | (1u << 2) | (1u << 3) | (1u << 4) | (1u << 11) |
    (1u << 12);
// Session/timing/all-car status packets can arrive at the game's frame rate,
// but Strategy only needs a recent state sample plus every lap/event/set change
// when reconstructing after a flashback. Retain these inputs in the engine's
// existing history at 4 Hz instead of duplicating every full JSON row.
static constexpr float kStrategyHistoryIntervalS = 0.25f;
static constexpr std::chrono::milliseconds kStrategyPublishInterval{100};

static uint64_t steadyClockMilliseconds() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

template <typename T>
static void updateAtomicMaximum(std::atomic<T>& target, T value) {
    T current = target.load(std::memory_order_relaxed);
    while (current < value && !target.compare_exchange_weak(
        current, value, std::memory_order_relaxed, std::memory_order_relaxed)) {}
}

static uint8_t rowTypeOf(std::string_view json) {
    static constexpr std::pair<std::string_view, uint8_t> TYPES[] = {
        {"telemetry", 1}, {"status", 2}, {"damage", 3}, {"lap", 4},
        {"session", 5}, {"race_event", 6}, {"timing", 7},
        {"participants", 8}, {"all_status", 9}, {"tyre_sets", 10},
        {"motion", 11}, {"motion_ex", 12}, {"positions", 13},
        {"session_history_fastest", 14}, {"strategy", 15},
    };
    static constexpr std::string_view TYPE_KEY = "\"type\":\"";
    const size_t key = json.find(TYPE_KEY);
    if (key == std::string_view::npos) return 0;
    const std::string_view value = json.substr(key + TYPE_KEY.size());
    for (const auto& [name, type] : TYPES)
        if (value.starts_with(name) && value.size() > name.size() &&
            value[name.size()] == '\"') return type;
    return 0;
}

static std::vector<uint8_t> typesInMask(uint32_t mask) {
    std::vector<uint8_t> out;
    for (uint8_t type = 1; type < 16; ++type)
        if (mask & (1u << type)) out.push_back(type);
    return out;
}

// The V6 fields the reader must load, in its convention (empty = every field),
// as the union of the host UI's and the paired phones' requests. A host that
// declared row families but no fields, or a legacy all-rows host, wants every
// field; a host with no consumers yet wants none, so an idle desktop does not
// widen a phone's list to everything.
static std::vector<uint8_t> readerV6Types(uint32_t hostRowMask,
                                          const std::vector<uint8_t>& hostTypes,
                                          const std::vector<uint8_t>& pairTypes) {
    if (hostRowMask != 0 && hostTypes.empty()) return {};
    std::set<uint8_t> merged(hostTypes.begin(), hostTypes.end());
    merged.insert(pairTypes.begin(), pairTypes.end());
    return {merged.begin(), merged.end()};
}

static std::string byteList(const std::vector<uint8_t>& values) {
    std::string out{"["};
    for (size_t i = 0; i < values.size(); ++i) {
        if (i != 0) out.push_back(',');
        out += std::to_string(values[i]);
    }
    out.push_back(']');
    return out;
}

static double scanJsonNumber(std::string_view json, std::string_view key,
                             double fallback = 0.0) {
    const size_t at = json.find(key);
    if (at == std::string_view::npos) return fallback;
    const char* value = json.data() + at + key.size();
    char* end = nullptr;
    const double parsed = std::strtod(value, &end);
    return end == value ? fallback : parsed;
}

// Rewrites (or inserts, for row types that lack it, e.g. positions/session)
// the top-level "session_time" of a raw JSON row to the playhead. A targeted
// string splice — the row is otherwise re-emitted verbatim.
static void setSessionTime(std::string& line, float t) {
    char num[32];
    std::snprintf(num, sizeof(num), "%.9g", (double)t);
    static const char KEY[] = "\"session_time\":";
    size_t k = line.find(KEY);
    if (k != std::string::npos) {
        size_t vs = k + sizeof(KEY) - 1;
        size_t ve = vs;
        while (ve < line.size() && line[ve] != ',' && line[ve] != '}') ++ve;
        line.replace(vs, ve - vs, num);
    } else {
        size_t brace = line.find('{');
        if (brace == std::string::npos) return;
        std::string ins = std::string(KEY) + num + ",";
        line.insert(brace + 1, ins);
    }
}

Engine::Engine(const Config& config, Sink* sink)
    : config_(config), sink_(sink),
      parser_(config.protocol, config.teamColorOverrides),
      writer_([sink](const std::string& operation, const std::string& message,
                     const std::string& path) {
          if (!sink) return;
          RecordingErrorRow row;
          row.operation = operation;
          row.message   = message;
          row.path      = path;
          sink->onRow(writeJson(row));
      }) {
    TRACE("Engine ctor: start");
    config_.teamColorOverrides = sanitizeTeamColorOverrides(config.teamColorOverrides);
    teamColorOverrides_ = std::make_shared<const TeamColorOverrides>(
        config_.teamColorOverrides);
    // Electron declares its visible consumers immediately after renderer
    // mount. Start that host closed so no telemetry can slip through before
    // the first aggregate subscription; JSON-only/Qt hosts keep legacy-all.
    if (config_.binaryPlayback) {
        consumerRowMask_ = 0;
        hostConsumerRowMask_ = 0;
    }
    liveHistory_ = std::make_unique<detail::LiveHistoryStore>();
    liveHistoryLastSample_.fill(-std::numeric_limits<float>::infinity());
    liveHistoryLastLap_.fill(-1);
    liveRetirementTimes_.fill(std::numeric_limits<float>::infinity());
    strategy_.setMinimumStops(config_.strategyMinimumStops);
    reader_.setStrategyMinimumStops(config_.strategyMinimumStops);
    strategy_.setTeamColorOverrides(config_.teamColorOverrides);
    reader_.setTeamColorOverrides(config_.teamColorOverrides);
    strategyThread_ = std::thread(&Engine::strategyLoop, this);
    enqueueLiveStrategyWork({StrategyWorkKind::Reset, liveStrategyGeneration_,
                             2025, config_.strategyMinimumStops, false, {}});
    writer_.setLogging(config.loggingEnabled, config.outputDirectory);
    TRACE("Engine ctor: writer_.setLogging done");
    pairServer_.configure({config.pairPort, config.pairName,
                           config.pairStateJson, config.pairEnabled},
        [this](const std::string& publicJson,
               const std::string& persistedJson) {
            if (sink_) sink_->onPairState(publicJson, persistedJson);
        },
        [this](uint32_t streamMask, const std::vector<uint8_t>& v6Types,
               bool refreshSnapshot) {
            setPairDataRequirements(streamMask, v6Types, refreshSnapshot);
        },
        [this](int currentLap, int comparisonLap, bool sectorDelta) {
            AnalysisLapProgress current;
            AnalysisLapProgress comparison;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!inPlayback_.load() ||
                    !reader_.getAnalysisLapProgress(currentLap, current) ||
                    !reader_.getAnalysisLapProgress(comparisonLap, comparison)) {
                    return std::string{};
                }
            }
            std::string response = writeJson(
                calculateLapDelta(current, comparison, sectorDelta));
            if (response.empty() || response.back() != '}') return std::string{};
            response.pop_back();
            response += ",\"currentStartSessionTime\":" +
                std::to_string(current.startSessionTime) +
                ",\"currentEndSessionTime\":" +
                std::to_string(current.endSessionTime) +
                ",\"currentProgress\":" + writeJson(current.points) + "}";
            return response;
        },
        [this](const std::string& message) {
            if (sink_) sink_->onPairDiagnostic(message);
        });
    emitRow(parser_.statusRow());
    TRACE("Engine ctor: emitRow(statusRow) done");
}

Engine::~Engine() {
    pairServer_.stop(false);
    stopPlaybackThread();
    udp_.stop();
    stopStrategyThread();
    liveHistory_.reset();
    writer_.closeActiveStream();
}

void Engine::emitRow(const std::string& json) {
    const uint16_t format = emittedFormat_.load(std::memory_order_acquire);
    if (format != 0 && rowTypeOf(json) == 8) {
        const auto overrides = std::atomic_load_explicit(
            &teamColorOverrides_, std::memory_order_acquire);
        const std::string resolved = applyTeamColorsToParticipantsJson(
            json, format, overrides ? *overrides : TeamColorOverrides{});
        pairServer_.publishRow(resolved);
        if (sink_) sink_->onRow(resolved);
        return;
    }
    pairServer_.publishRow(json);
    if (sink_) sink_->onRow(json);
}

void Engine::emitRows(const std::vector<std::string>& rows) {
    if (rows.empty()) return;
    std::vector<std::string> resolvedRows;
    resolvedRows.reserve(rows.size());
    const uint16_t format = emittedFormat_.load(std::memory_order_acquire);
    const auto overrides = std::atomic_load_explicit(
        &teamColorOverrides_, std::memory_order_acquire);
    for (const auto& row : rows) {
        if (format != 0 && rowTypeOf(row) == 8) {
            resolvedRows.push_back(applyTeamColorsToParticipantsJson(
                row, format, overrides ? *overrides : TeamColorOverrides{}));
        } else {
            resolvedRows.push_back(row);
        }
        pairServer_.publishRow(resolvedRows.back());
    }
    if (sink_) sink_->onRows(resolvedRows);
}

void Engine::emitBinary(const uint8_t* data, size_t length) {
    pairServer_.publishBinary(data, length);
    if (sink_) sink_->onBinary(data, length);
}

bool Engine::pairStart(std::string* errorOut) {
    return pairServer_.start(errorOut);
}

void Engine::pairStop(bool persistDisabled) {
    pairServer_.stop(persistDisabled);
}

void Engine::pairOpenWindow() { pairServer_.openPairingWindow(); }
void Engine::pairCloseWindow() { pairServer_.closePairingWindow(); }

void Engine::pairRemoveDevice(const std::string& id) {
    pairServer_.removeDevice(id);
}

std::string Engine::pairStateJson() const {
    return pairServer_.publicStateJson();
}

std::string Engine::pairPersistedStateJson() const {
    return pairServer_.persistedStateJson();
}

// ── Live ─────────────────────────────────────────────────────────────────────

bool Engine::startUdp() {
    TRACE("Engine::startUdp: start");
    bool ok = udp_.start(config_.port, config_.bindAddress,
                      [this](const uint8_t* d, int n) { onDatagram(d, n); },
                      config_.udpForwardTargets);
    TRACE("Engine::startUdp: udp_.start returned");
    return ok;
}

bool Engine::restartUdp(uint16_t port, const std::string& bindAddress) {
    udp_.stop();
    {
        std::lock_guard<std::mutex> lk(mutex_);
        config_.port        = port;
        config_.bindAddress = bindAddress;
        parser_.reset();
        strategy_.reset();
        ++liveStrategyGeneration_;
        enqueueLiveStrategyWork({StrategyWorkKind::Reset, liveStrategyGeneration_,
                                 2025, config_.strategyMinimumStops, false, {}});
        liveLatestRows_ = {};
        liveHistory_->reset();
        liveHistoryLastSample_.fill(-std::numeric_limits<float>::infinity());
        liveHistoryLastLap_.fill(-1);
        liveRetirementTimes_.fill(std::numeric_limits<float>::infinity());
        liveHistorySequence_ = 0;
        liveSessionTime_ = 0.0f;
        liveLapStart_ = 0.0f;
        liveLapNum_ = 0;
        lastStrategyJson_.clear();
    }
    return udp_.start(port, bindAddress,
                      [this](const uint8_t* d, int n) { onDatagram(d, n); },
                      config_.udpForwardTargets);
}

std::string Engine::udpLastError() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return udp_.lastError();
}

void Engine::rewindLiveTimeline(float sessionTime, uint16_t format) {
    liveHistory_->rewind(sessionTime);
    StrategyWork rollback;
    rollback.kind = StrategyWorkKind::Rollback;
    rollback.generation = ++liveStrategyGeneration_;
    rollback.format = format;
    rollback.minimumStops = config_.strategyMinimumStops;
    rollback.rebuildThrough = sessionTime;
    enqueueLiveStrategyWork(std::move(rollback));
    liveLatestRows_[kStrategyRowType].clear();
    lastStrategyJson_.clear();

    // State rows newer than the target must not leak back into a page restored
    // after the rewind. Historical families can be restored from their tails.
    for (size_t typeIndex = 1; typeIndex < liveLatestRows_.size(); ++typeIndex) {
        const auto type = static_cast<uint8_t>(typeIndex);
        if (!((kHistoricalRowMask | kStrategyDependencyMask) & (1u << type))) continue;
        liveLatestRows_[typeIndex] = liveHistory_->latestJson(type, sessionTime);
    }
    liveLapNum_ = liveHistory_->currentLap();
    liveLapStart_ = liveHistory_->currentLapStart();
    liveSessionTime_ = sessionTime;
    liveHistoryLastSample_.fill(sessionTime);
    liveHistoryLastLap_.fill(liveLapNum_);
    for (float& retirementTime : liveRetirementTimes_)
        if (retirementTime > sessionTime)
            retirementTime = std::numeric_limits<float>::infinity();
}

void Engine::enqueueLiveStrategyWork(StrategyWork work) {
    work.queuedAtMs = steadyClockMilliseconds();
    size_t incomingJsonBytes = 0;
    for (const auto& row : work.rows)
        if (row.json) incomingJsonBytes += row.json->size();
    strategyInputRowsEnqueued_.fetch_add(work.rows.size(), std::memory_order_relaxed);
    strategyInputJsonBytesEnqueued_.fetch_add(incomingJsonBytes, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(strategyWorkMutex_);
        if (strategyStop_) return;
        const auto updateQueuePeaks = [this] {
            size_t rows = 0;
            size_t retainedBytes = 0;
            for (const auto& queued : strategyWorkQueue_) {
                rows += queued.rows.size();
                retainedBytes += queued.rows.capacity() * sizeof(LiveJsonHistoryRow) +
                    queued.playbackPath.capacity() + 1;
                for (const auto& row : queued.rows)
                    if (row.json) retainedBytes += row.json->capacity() + 1;
            }
            updateAtomicMaximum(strategyPeakQueuedRows_, rows);
            updateAtomicMaximum(strategyPeakQueuedRetainedBytes_, retainedBytes);
        };
        // Rollback needs the preceding inputs and checkpoints, even when the
        // worker is behind UDP. Preserve that prefix and process it in order.
        // Only a reset supersedes all queued work from the old live session.
        // Playback rebuilds are also latest-only: a newer seek must not wait
        // behind reconstruction for a cursor that can no longer be displayed.
        if (work.kind == StrategyWorkKind::Reset ||
            work.kind == StrategyWorkKind::PlaybackRebuild) {
            strategyWorkQueue_.erase(
                std::remove_if(strategyWorkQueue_.begin(), strategyWorkQueue_.end(),
                    [&](const StrategyWork& queued) {
                        return work.kind == StrategyWorkKind::PlaybackRebuild
                            ? queued.kind == StrategyWorkKind::PlaybackRebuild &&
                                queued.generation < work.generation
                            : queued.kind != StrategyWorkKind::PlaybackRebuild &&
                                queued.generation < work.generation;
                    }),
                strategyWorkQueue_.end());
        }
        if (work.kind == StrategyWorkKind::Update &&
            !strategyWorkQueue_.empty() &&
            strategyWorkQueue_.back().kind == StrategyWorkKind::Update &&
            strategyWorkQueue_.back().generation == work.generation) {
            auto& pending = strategyWorkQueue_.back();
            for (auto& incoming : work.rows) {
                const uint8_t incomingType = incoming.json
                    ? rowTypeOf(*incoming.json) : 0;
                // Race events are edge-triggered and must never be coalesced.
                if (incomingType != 6) {
                    auto previous = std::find_if(pending.rows.rbegin(), pending.rows.rend(),
                        [&](const LiveJsonHistoryRow& row) {
                            return row.json && rowTypeOf(*row.json) == incomingType;
                        });
                    if (previous != pending.rows.rend()) {
                        const bool crossedLap = incomingType == 4 &&
                            scanJsonNumber(*previous->json, "\"lap_num\":", -1) !=
                            scanJsonNumber(*incoming.json, "\"lap_num\":", -1);
                        if (!crossedLap) {
                            *previous = std::move(incoming);
                            continue;
                        }
                    }
                }
                pending.rows.push_back(std::move(incoming));
            }
            pending.format = work.format;
            pending.minimumStops = work.minimumStops;
            pending.forceSnapshot = pending.forceSnapshot || work.forceSnapshot;
            updateQueuePeaks();
            strategyWorkCv_.notify_one();
            return;
        }
        strategyWorkQueue_.push_back(std::move(work));
        updateQueuePeaks();
    }
    strategyWorkCv_.notify_one();
}

void Engine::strategyLoop() {
    detail::StrategyRollback liveRollback;
    auto& liveStrategy = liveRollback.processor();
    const auto publishMemoryStats = [&] {
        const auto rollback = liveRollback.memoryStats();
        std::lock_guard<std::mutex> lock(strategyMemoryStatsMutex_);
        publishedLiveStrategyMemoryStats_ = liveStrategy.memoryStats();
        publishedStrategyRollbackMemoryStats_ = {
            rollback.retainedBytes, rollback.checkpoints, rollback.rows,
            rollback.rollbacks, rollback.replayedRows, rollback.fallbacks};
    };
    TnrdReader playbackReader;
    std::string playbackReaderPath;
    uint64_t activeGeneration = 0;
    auto lastPublished = std::chrono::steady_clock::time_point::min();

    for (;;) {
        std::deque<StrategyWork> work;
        {
            std::unique_lock<std::mutex> lock(strategyWorkMutex_);
            strategyWorkCv_.wait(lock, [this] {
                return strategyStop_ || !strategyWorkQueue_.empty();
            });
            if (strategyStop_ && strategyWorkQueue_.empty()) return;
            work.swap(strategyWorkQueue_);
        }

        size_t activeRows = 0;
        size_t activeJsonBytes = 0;
        size_t activeRetainedBytes = 0;
        for (const auto& item : work) {
            activeRows += item.rows.size();
            activeRetainedBytes += item.rows.capacity() * sizeof(LiveJsonHistoryRow) +
                item.playbackPath.capacity() + 1;
            for (const auto& row : item.rows) {
                if (!row.json) continue;
                activeJsonBytes += row.json->size();
                activeRetainedBytes += row.json->capacity() + 1;
            }
        }
        strategyActiveWorkItems_.store(work.size(), std::memory_order_relaxed);
        strategyActiveRows_.store(activeRows, std::memory_order_relaxed);
        strategyActiveJsonBytes_.store(activeJsonBytes, std::memory_order_relaxed);
        strategyActiveRetainedBytes_.store(activeRetainedBytes, std::memory_order_relaxed);

        bool changed = false;
        bool forceSnapshot = false;
        for (auto& item : work) {
            size_t processedJsonBytes = 0;
            for (const auto& row : item.rows)
                if (row.json) processedJsonBytes += row.json->size();
            strategyRowsProcessed_.fetch_add(item.rows.size(), std::memory_order_relaxed);
            strategyJsonBytesProcessed_.fetch_add(processedJsonBytes, std::memory_order_relaxed);
            if (item.kind == StrategyWorkKind::PlaybackRebuild) {
                const auto cancelled = [this, generation = item.generation,
                                        seekRequestId = item.seekRequestId] {
                    return playbackStrategyGeneration_.load(std::memory_order_acquire) != generation ||
                        (seekRequestId != 0 && latestSeekRequestId_.load(std::memory_order_acquire) != seekRequestId);
                };
                if (cancelled()) continue;

                if (playbackReaderPath != item.playbackPath) {
                    playbackReader.close();
                    HeaderRow header;
                    if (!playbackReader.load(item.playbackPath, header)) {
                        playbackReaderPath.clear();
                        std::lock_guard<std::mutex> lock(mutex_);
                        if (playbackStrategyPendingGeneration_ == item.generation) {
                            playbackStrategyPending_ = false;
                            playbackStrategyPendingRows_.clear();
                        }
                        continue;
                    }
                    playbackReaderPath = item.playbackPath;
                }
                playbackReader.setStrategyMinimumStops(item.minimumStops);
                const auto teamColors = std::atomic_load_explicit(
                    &teamColorOverrides_, std::memory_order_acquire);
                playbackReader.setTeamColorOverrides(
                    teamColors ? *teamColors : TeamColorOverrides{});

                StrategyProcessor rebuilt;
                (void)playbackReader.strategySnapshotAt(
                    item.rebuildThrough, &rebuilt, cancelled);
                if (cancelled()) continue;

                std::string json;
                bool emit = false;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (!inPlayback_.load() || !playbackStrategyPending_ ||
                        playbackStrategyPendingGeneration_ != item.generation ||
                        playbackStrategyGeneration_.load(std::memory_order_acquire) != item.generation ||
                        (item.seekRequestId != 0 &&
                         (latestSeekRequestId_.load(std::memory_order_acquire) != item.seekRequestId ||
                          appliedSeekRequestId_ != item.seekRequestId))) {
                        continue;
                    }
                    // Playback may already have advanced beyond the requested
                    // cursor. Fold every dependency row delivered since the
                    // rebuild was queued into the new processor before making
                    // it authoritative, so the asynchronous result never
                    // rewinds Strategy relative to the playhead.
                    for (const auto& row : playbackStrategyPendingRows_)
                        rebuilt.ingestJson(row);
                    playbackStrategyPendingRows_.clear();
                    strategy_ = std::move(rebuilt);
                    json = strategy_.snapshotJson();
                    strategySnapshotsGenerated_.fetch_add(1, std::memory_order_relaxed);
                    strategySnapshotJsonBytesGenerated_.fetch_add(json.size(), std::memory_order_relaxed);
                    strategyLastSnapshotJsonBytes_.store(json.size(), std::memory_order_relaxed);
                    strategyLastSnapshotJsonCapacityBytes_.store(json.capacity() + 1, std::memory_order_relaxed);
                    updateAtomicMaximum(strategyPeakSnapshotJsonBytes_, json.size());
                    liveLatestRows_[kStrategyRowType] = json;
                    lastStrategyJson_ = json;
                    playbackStrategyPending_ = false;
                    emit = (consumerRowMask_ & kStrategyRowBit) != 0;
                }
                if (emit) {
                    strategySnapshotsEmitted_.fetch_add(1, std::memory_order_relaxed);
                    emitRow(json);
                }
                continue;
            }
            if (item.generation < activeGeneration) continue;
            if (item.kind == StrategyWorkKind::Reset ||
                (item.generation > activeGeneration &&
                 item.kind != StrategyWorkKind::Rollback)) {
                liveRollback.reset();
            }
            activeGeneration = item.generation;
            const auto teamColors = std::atomic_load_explicit(
                &teamColorOverrides_, std::memory_order_acquire);
            const auto configure = [&] {
                liveStrategy.setFormat(item.format);
                liveStrategy.setTeamColorOverrides(
                    teamColors ? *teamColors : TeamColorOverrides{});
                liveStrategy.setMinimumStops(item.minimumStops);
            };
            configure();
            if (item.kind == StrategyWorkKind::Rollback) {
                if (!liveRollback.rollback(item.rebuildThrough)) {
                    // Exceptional deep rewinds stream one lap at a time. Never
                    // materialize the full race's expanded JSON in a work item.
                    liveRollback.reset();
                    configure();
                    liveHistory_->forEachStrategyRow(item.rebuildThrough,
                        [&](const detail::LiveHistoryJsonRow& row) {
                            liveRollback.ingest(row.sessionTime, row.json);
                            strategyRowsProcessed_.fetch_add(1, std::memory_order_relaxed);
                            if (row.json) strategyJsonBytesProcessed_.fetch_add(
                                row.json->size(), std::memory_order_relaxed);
                        },
                        [&](size_t rows, size_t jsonBytes, size_t retainedBytes) {
                            strategyActiveRows_.store(activeRows + rows, std::memory_order_relaxed);
                            strategyActiveJsonBytes_.store(activeJsonBytes + jsonBytes, std::memory_order_relaxed);
                            strategyActiveRetainedBytes_.store(activeRetainedBytes + retainedBytes,
                                                              std::memory_order_relaxed);
                        });
                }
                // A checkpoint may have been saved before a settings change.
                configure();
                liveRollback.configurationChanged();
                changed = true;
                forceSnapshot = true;
            }
            if (item.kind == StrategyWorkKind::Configure)
                liveRollback.configurationChanged();
            if (item.rows.size() > 1) {
                std::stable_sort(item.rows.begin(), item.rows.end(),
                    [](const LiveJsonHistoryRow& a, const LiveJsonHistoryRow& b) {
                        return a.sequence < b.sequence;
                    });
            }
            for (const auto& row : item.rows) {
                liveRollback.ingest(row.sessionTime, row.json);
                changed = true;
            }
            forceSnapshot = forceSnapshot || item.forceSnapshot;
            if (item.kind == StrategyWorkKind::Configure) changed = true;
        }
        publishMemoryStats();
        strategyActiveWorkItems_.store(0, std::memory_order_relaxed);
        strategyActiveRows_.store(0, std::memory_order_relaxed);
        strategyActiveJsonBytes_.store(0, std::memory_order_relaxed);
        strategyActiveRetainedBytes_.store(0, std::memory_order_relaxed);

        bool visible = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            visible = !inPlayback_.load() &&
                activeGeneration == liveStrategyGeneration_ &&
                (consumerRowMask_ & kStrategyRowBit) != 0;
        }
        const auto now = std::chrono::steady_clock::now();
        if (!changed || !visible) continue;
        if (!forceSnapshot &&
            lastPublished != std::chrono::steady_clock::time_point::min() &&
            now - lastPublished < kStrategyPublishInterval) {
            // This is the Strategy worker, never the UDP thread. Waiting here
            // provides a true trailing-edge publish: the last update before a
            // pause is not silently discarded merely because it arrived inside
            // the display cadence window. New UDP work coalesces in the queue.
            std::this_thread::sleep_until(lastPublished + kStrategyPublishInterval);
        }

        std::string json = liveRollback.snapshotJson();
        publishMemoryStats();
        strategySnapshotsGenerated_.fetch_add(1, std::memory_order_relaxed);
        strategySnapshotJsonBytesGenerated_.fetch_add(json.size(), std::memory_order_relaxed);
        strategyLastSnapshotJsonBytes_.store(json.size(), std::memory_order_relaxed);
        strategyLastSnapshotJsonCapacityBytes_.store(json.capacity() + 1, std::memory_order_relaxed);
        updateAtomicMaximum(strategyPeakSnapshotJsonBytes_, json.size());
        bool emit = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!inPlayback_.load() && activeGeneration == liveStrategyGeneration_ &&
                (consumerRowMask_ & kStrategyRowBit)) {
                liveLatestRows_[kStrategyRowType] = json;
                if (forceSnapshot || json != lastStrategyJson_) {
                    lastStrategyJson_ = json;
                    emit = true;
                }
            }
        }
        if (emit) {
            strategySnapshotsEmitted_.fetch_add(1, std::memory_order_relaxed);
            emitRow(json);
        }
        lastPublished = std::chrono::steady_clock::now();
    }
}

void Engine::stopStrategyThread() {
    playbackStrategyGeneration_.fetch_add(1, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(strategyWorkMutex_);
        strategyStop_ = true;
        strategyWorkQueue_.clear();
    }
    strategyWorkCv_.notify_all();
    if (strategyThread_.joinable()) strategyThread_.join();
}

void Engine::ingestStrategyRow(const std::string& json) {
    const uint8_t type = rowTypeOf(json);
    if (!(kStrategyDependencyMask & (1u << type))) return;
    strategy_.ingestJson(json);
}

void Engine::emitStrategy(bool force) {
    std::string json = strategy_.snapshotJson();
    liveLatestRows_[kStrategyRowType] = json;
    if ((consumerRowMask_ & kStrategyRowBit) && (force || json != lastStrategyJson_)) {
        lastStrategyJson_ = json;
        emitRow(json);
    }
}

bool Engine::preparePlaybackStrategyRebuildLocked(float target, StrategyWork& work) {
    const uint64_t generation = playbackStrategyGeneration_.fetch_add(
        1, std::memory_order_acq_rel) + 1;
    playbackStrategyPendingRows_.clear();
    playbackStrategyPendingGeneration_ = generation;
    playbackStrategyPending_ = inPlayback_.load() &&
        !playbackPath_.empty() && (consumerRowMask_ & kStrategyRowBit) != 0;
    if (!playbackStrategyPending_) return false;

    work.kind = StrategyWorkKind::PlaybackRebuild;
    work.generation = generation;
    work.seekRequestId = appliedSeekRequestId_;
    work.format = emittedFormat_.load(std::memory_order_acquire);
    work.minimumStops = config_.strategyMinimumStops;
    work.rebuildThrough = target;
    work.playbackPath = playbackPath_;
    return true;
}

void Engine::requestPlaybackStrategyRebuildLocked(float target) {
    StrategyWork work;
    if (!preparePlaybackStrategyRebuildLocked(target, work)) return;
    enqueueLiveStrategyWork(std::move(work));
}

Engine::LiveDiagnostics Engine::liveDiagnostics() const {
    std::lock_guard<std::mutex> lk(mutex_);
    LiveDiagnostics snapshot = liveDiagnostics_;
    snapshot.udpRunning = udp_.isRunning();
    snapshot.inPlayback = inPlayback_.load();
    snapshot.recording = writer_.isRecording();
    snapshot.consumerRowMask = consumerRowMask_;
    snapshot.consumerHistoryMask = consumerHistoryMask_;
    snapshot.consumerWindowSeconds = consumerWindowSeconds_;
    return snapshot;
}

Engine::LiveHistoryMemoryStats Engine::liveHistoryMemoryStats() const {
    const auto source = liveHistory_->memoryStats();
    LiveHistoryMemoryStats result;
    result.retainedBytes = source.retainedBytes;
    result.lapCount = source.lapCount;
    result.pinnedLapCount = source.pinnedLapCount;
    result.compressedLapCount = source.compressedLapCount;
    result.busyLapCount = source.busyLapCount;
    result.packedBytes = source.packedBytes;
    result.packedCapacityBytes = source.packedCapacityBytes;
    result.jsonRows = source.jsonRows;
    result.jsonPayloadBytes = source.jsonPayloadBytes;
    result.jsonPayloadCapacityBytes = source.jsonPayloadCapacityBytes;
    result.jsonContainerCapacityBytes = source.jsonContainerCapacityBytes;
    result.sequenceEntries = source.sequenceEntries;
    result.sequenceCapacityBytes = source.sequenceCapacityBytes;
    result.compressedPlainBytes = source.compressedPlainBytes;
    result.compressedBytes = source.compressedBytes;
    result.compressedCapacityBytes = source.compressedCapacityBytes;
    result.queuedJobs = source.queuedJobs;
    result.activeJobKind = source.activeJobKind;
    result.compressionJobs = source.compressionJobs;
    result.compressedFamilies = source.compressedFamilies;
    result.compressionPlainBytesProcessed = source.compressionPlainBytesProcessed;
    result.compressionPlainBufferBytesAllocated =
        source.compressionPlainBufferBytesAllocated;
    result.compressionBufferBytesAllocated = source.compressionBufferBytesAllocated;
    result.compressedOutputBytesAllocated = source.compressedOutputBytesAllocated;
    result.lastCompressionPlainBytes = source.lastCompressionPlainBytes;
    result.lastCompressionBufferBytes = source.lastCompressionBufferBytes;
    result.lastCompressionScratchBytes = source.lastCompressionScratchBytes;
    result.peakCompressionPlainBytes = source.peakCompressionPlainBytes;
    result.peakCompressionBufferBytes = source.peakCompressionBufferBytes;
    result.peakCompressionScratchBytes = source.peakCompressionScratchBytes;
    result.decompressionJobs = source.decompressionJobs;
    result.decompressionBufferBytesAllocated = source.decompressionBufferBytesAllocated;
    result.lastDecompressionBufferBytes = source.lastDecompressionBufferBytes;
    result.peakDecompressionBufferBytes = source.peakDecompressionBufferBytes;
    result.rangeJobs = source.rangeJobs;
    return result;
}

Engine::StrategyMemoryStats Engine::strategyMemoryStats() const {
    StrategyMemoryStats stats;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stats.subscribed = (consumerRowMask_ & kStrategyRowBit) != 0;
        stats.cacheCapacityBytes = lastStrategyJson_.capacity() + 1 +
            liveLatestRows_[kStrategyRowType].capacity() + 1 +
            playbackStrategyPendingRows_.capacity() * sizeof(std::string);
        for (const auto& row : playbackStrategyPendingRows_)
            stats.cacheCapacityBytes += row.capacity() + 1;
        if (inPlayback_.load()) stats.processor = strategy_.memoryStats();
    }
    {
        std::lock_guard<std::mutex> lock(strategyMemoryStatsMutex_);
        if (!inPlayback_.load()) stats.processor = publishedLiveStrategyMemoryStats_;
        stats.rollback = publishedStrategyRollbackMemoryStats_;
    }
    {
        std::lock_guard<std::mutex> lock(strategyWorkMutex_);
        const uint64_t now = steadyClockMilliseconds();
        uint64_t oldest = 0;
        stats.queuedWorkItems = strategyWorkQueue_.size();
        for (const auto& work : strategyWorkQueue_) {
            stats.queuedRows += work.rows.size();
            stats.queuedRetainedBytes +=
                work.rows.capacity() * sizeof(LiveJsonHistoryRow) +
                work.playbackPath.capacity() + 1;
            for (const auto& row : work.rows) {
                if (!row.json) continue;
                stats.queuedJsonBytes += row.json->size();
                stats.queuedRetainedBytes += row.json->capacity() + 1;
            }
            if (work.queuedAtMs != 0 && (oldest == 0 || work.queuedAtMs < oldest))
                oldest = work.queuedAtMs;
        }
        if (oldest != 0 && now >= oldest) stats.oldestQueuedWorkAgeMs = now - oldest;
    }
    stats.peakQueuedRows = strategyPeakQueuedRows_.load(std::memory_order_relaxed);
    stats.peakQueuedRetainedBytes =
        strategyPeakQueuedRetainedBytes_.load(std::memory_order_relaxed);
    stats.activeWorkItems = strategyActiveWorkItems_.load(std::memory_order_relaxed);
    stats.activeRows = strategyActiveRows_.load(std::memory_order_relaxed);
    stats.activeJsonBytes = strategyActiveJsonBytes_.load(std::memory_order_relaxed);
    stats.activeRetainedBytes = strategyActiveRetainedBytes_.load(std::memory_order_relaxed);
    stats.inputRowsEnqueued = strategyInputRowsEnqueued_.load(std::memory_order_relaxed);
    stats.inputJsonBytesEnqueued =
        strategyInputJsonBytesEnqueued_.load(std::memory_order_relaxed);
    stats.rowsProcessed = strategyRowsProcessed_.load(std::memory_order_relaxed);
    stats.jsonBytesProcessed = strategyJsonBytesProcessed_.load(std::memory_order_relaxed);
    stats.snapshotsGenerated = strategySnapshotsGenerated_.load(std::memory_order_relaxed);
    stats.snapshotsEmitted = strategySnapshotsEmitted_.load(std::memory_order_relaxed);
    stats.snapshotJsonBytesGenerated =
        strategySnapshotJsonBytesGenerated_.load(std::memory_order_relaxed);
    stats.lastSnapshotJsonBytes =
        strategyLastSnapshotJsonBytes_.load(std::memory_order_relaxed);
    stats.lastSnapshotJsonCapacityBytes =
        strategyLastSnapshotJsonCapacityBytes_.load(std::memory_order_relaxed);
    stats.peakSnapshotJsonBytes =
        strategyPeakSnapshotJsonBytes_.load(std::memory_order_relaxed);
    stats.retainedBytes = stats.cacheCapacityBytes + stats.queuedRetainedBytes +
        stats.activeRetainedBytes + stats.processor.retainedBytes + stats.rollback.retainedBytes;
    return stats;
}

Engine::RuntimeMemoryStats Engine::runtimeMemoryStats() const {
    RuntimeMemoryStats stats;
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& row : dupCache_) {
        stats.duplicateCacheUsedBytes += row.size();
        stats.duplicateCacheCapacityBytes += row.capacity() + 1;
    }
    for (size_t type = 0; type < liveLatestRows_.size(); ++type) {
        // Strategy's latest row is already included in StrategyMemoryStats.
        if (type == kStrategyRowType) continue;
        stats.latestRowCacheUsedBytes += liveLatestRows_[type].size();
        stats.latestRowCacheCapacityBytes += liveLatestRows_[type].capacity() + 1;
    }
    stats.playbackPathCapacityBytes = playbackPath_.capacity() + 1;
    stats.datagramsProcessed = runtimeDatagramsProcessed_;
    stats.datagramBytesProcessed = runtimeDatagramBytesProcessed_;
    stats.parserRowsProduced = runtimeParserRowsProduced_;
    stats.parserControlRowsProduced = runtimeParserControlRowsProduced_;
    stats.parserHotJsonRowsProduced = runtimeParserHotJsonRowsProduced_;
    stats.parserJsonBytesProduced = runtimeParserJsonBytesProduced_;
    stats.parserBinaryBytesProduced = runtimeParserBinaryBytesProduced_;
    stats.parserResultCapacityBytesAllocated = runtimeParserResultCapacityAllocated_;
    stats.lastParserResultCapacityBytes = runtimeLastParserResultCapacity_;
    stats.peakParserResultCapacityBytes = runtimePeakParserResultCapacity_;
    stats.filteredBinaryBatches = runtimeFilteredBinaryBatches_;
    stats.filteredBinaryBytesProduced = runtimeFilteredBinaryBytesProduced_;
    stats.filteredBinaryCapacityBytesAllocated = runtimeFilteredBinaryCapacityAllocated_;
    stats.lastFilteredBinaryCapacityBytes = runtimeLastFilteredBinaryCapacity_;
    stats.peakFilteredBinaryCapacityBytes = runtimePeakFilteredBinaryCapacity_;
    stats.retainedBytes = stats.duplicateCacheCapacityBytes +
        stats.latestRowCacheCapacityBytes + stats.playbackPathCapacityBytes;
    return stats;
}

TnrdWriter::MemoryStats Engine::writerMemoryStats() const {
    return writer_.memoryStats();
}

void Engine::setDiagnosticsEnabled(bool enabled) {
    std::lock_guard<std::mutex> lk(mutex_);
    if (enabled && !liveDiagnosticsEnabled_) liveDiagnostics_ = {};
    liveDiagnosticsEnabled_ = enabled;
}

void Engine::onDatagram(const uint8_t* data, int length) {
    if (inPlayback_.load()) return;

    std::lock_guard<std::mutex> lk(mutex_);
    const bool collectDiagnostics = liveDiagnosticsEnabled_;
    if (collectDiagnostics) {
        liveDiagnostics_.datagrams++;
        if (length > 0) liveDiagnostics_.bytes += static_cast<uint64_t>(length);
        liveDiagnostics_.lastDatagramLength = length;
        if (length < 29) {
            liveDiagnostics_.tooShort++;
        } else {
            const uint16_t incoming = static_cast<uint16_t>(data[0]) |
                (static_cast<uint16_t>(data[1]) << 8);
            const uint8_t packetId = data[6];
            liveDiagnostics_.lastIncomingFormat = incoming;
            liveDiagnostics_.lastPacketId = packetId;
            liveDiagnostics_.packetIds[packetId <= 16 ? packetId : 17]++;
            if (incoming == 2024) liveDiagnostics_.format2024++;
            else if (incoming == 2025) liveDiagnostics_.format2025++;
            else if (incoming == 2026) liveDiagnostics_.format2026++;
            else liveDiagnostics_.unsupportedFormat++;
        }
    }
    std::string ts = isoTimestamp();

    // Only touch the recording pipeline when logging is enabled. When it's off
    // this skips a full datagram copy + per-row json enqueue + disk-thread wakeup
    // per packet, and the hot 60 Hz rows are never serialised to JSON at all
    // (parser produces only the binary form) — unless a consumer asked for the
    // hot rows as JSON (config_.hotRowsAsJson), in which case we also need them.
    const bool recording   = writer_.isRecording();
    const bool wantHotJson = recording || config_.hotRowsAsJson;
    // Electron retains chart-capable families only as compact native history
    // while hidden, so they can be backfilled without having crossed N-API or
    // been decoded into renderer objects. Other packet bodies are skipped.
    const uint32_t parserMask = recording ? 0xFFFFFFFFu : consumerRowMask_ | kStrategyDependencyMask |
        (config_.binaryPlayback ? kHistoricalRowMask : 0u);
    Parser::Result r = parser_.feed(data, length, ts, wantHotJson, parserMask);
    size_t parserJsonBytes = 0;
    size_t parserResultCapacity =
        r.rows.capacity() * sizeof(std::string) +
        r.control.capacity() * sizeof(std::string) +
        r.hotJson.capacity() * sizeof(std::string) +
        r.binary.capacity();
    const auto countParserStrings = [&](const std::vector<std::string>& rows) {
        for (const auto& row : rows) {
            parserJsonBytes += row.size();
            parserResultCapacity += row.capacity() + 1;
        }
    };
    countParserStrings(r.rows);
    countParserStrings(r.control);
    countParserStrings(r.hotJson);
    ++runtimeDatagramsProcessed_;
    if (length > 0) runtimeDatagramBytesProcessed_ += static_cast<uint64_t>(length);
    runtimeParserRowsProduced_ += r.rows.size();
    runtimeParserControlRowsProduced_ += r.control.size();
    runtimeParserHotJsonRowsProduced_ += r.hotJson.size();
    runtimeParserJsonBytesProduced_ += parserJsonBytes;
    runtimeParserBinaryBytesProduced_ += r.binary.size();
    runtimeParserResultCapacityAllocated_ += parserResultCapacity;
    runtimeLastParserResultCapacity_ = parserResultCapacity;
    runtimePeakParserResultCapacity_ = std::max(
        runtimePeakParserResultCapacity_, parserResultCapacity);
    if (r.format != 0) emittedFormat_.store(r.format, std::memory_order_release);
    if (r.format != 0) liveStrategyFormat_ = r.format;
    if (r.format != 0) pairServer_.noteSession(r.sessionUid);
    if (collectDiagnostics) {
        liveDiagnostics_.lastSessionTime = r.sessionTime;
        liveDiagnostics_.rowsProduced += static_cast<uint64_t>(r.rows.size());
        liveDiagnostics_.binaryBytesProduced += static_cast<uint64_t>(r.binary.size());
    }

    for (const auto& c : r.control) emitRow(c);
    if (r.dropped) {
        if (collectDiagnostics) liveDiagnostics_.parserDropped++;
        return;
    }
    if (collectDiagnostics) {
        liveDiagnostics_.accepted++;
        if (r.rows.empty() && r.binary.empty() && r.control.empty())
            liveDiagnostics_.noOutput++;
    }

    const float timelineTime = r.rewindSessionTime.value_or(r.sessionTime);
    if (std::isfinite(timelineTime) && timelineTime >= 0.0f) {
        // Prefer FLBK's exact target. If the event is absent/lost, retain the
        // session-time regression detector with a grace window for slightly
        // stale packets delivered after a pause or scheduling stall.
        if (r.rewindSessionTime || timelineTime < liveSessionTime_ - 0.2f)
            rewindLiveTimeline(timelineTime, r.format);
        else
            liveSessionTime_ = std::max(liveSessionTime_, timelineTime);
    }

    // RTMT is a state transition, not a repeatable notification: a car can
    // retire only once on a surviving timeline. Filter repeats before they
    // reach recording, live history, Strategy, paired clients, or the UI.
    r.rows.erase(std::remove_if(r.rows.begin(), r.rows.end(),
        [this](const std::string& row) {
            if (rowTypeOf(row) != 6 ||
                row.find("\"code\":\"RTMT\"") == std::string::npos) return false;
            const int carIdx = static_cast<int>(scanJsonNumber(
                row, "\"car_idx\":", -1.0));
            const float rowTime = static_cast<float>(scanJsonNumber(
                row, "\"session_time\":", -1.0));
            if (carIdx < 0 || carIdx >= static_cast<int>(liveRetirementTimes_.size()) ||
                !std::isfinite(rowTime) || rowTime < 0.0f) return false;
            if (std::isfinite(liveRetirementTimes_[carIdx])) return true;
            liveRetirementTimes_[carIdx] = rowTime;
            return false;
        }), r.rows.end());

    if (recording) {
        if (r.rewindSessionTime) writer_.rewind(*r.rewindSessionTime);
        writer_.notePacket(r.format, r.packetId, timelineTime, data, length);
        for (const auto& row : r.rows)    writer_.record(row, timelineTime);
        for (const auto& hj  : r.hotJson) writer_.record(hj, timelineTime);
    }

    // Keep one native latest-state row even while its page is hidden, then only
    // forward subscribed families. Recording above remains completely unmasked.
    bool strategyInput = false;
    std::vector<LiveJsonHistoryRow> strategyRows;
    for (const auto& row : r.rows) {
        const uint8_t type = rowTypeOf(row);
        const bool isStrategyInput = (kStrategyDependencyMask & (1u << type)) != 0;
        strategyInput = strategyInput || isStrategyInput;
        std::shared_ptr<const std::string> retainedRow;
        LiveJsonHistoryRow strategyRow;
        const float rowTime = static_cast<float>(scanJsonNumber(
            row, "\"session_time\":", r.sessionTime));
        if (config_.binaryPlayback && type == 4) {
            const int nextLap = static_cast<int>(scanJsonNumber(
                row, "\"lap_num\":", liveLapNum_));
            const double currentLapMs = scanJsonNumber(
                row, "\"current_lap_ms\":", 0.0);
            const float nextLapStart = rowTime >= 0.0f && currentLapMs >= 0.0
                ? rowTime - static_cast<float>(currentLapMs / 1000.0)
                : rowTime;
            if (nextLap > 0 && nextLap != liveLapNum_) {
                const int completedLapMs = nextLap > liveLapNum_
                    ? static_cast<int>(scanJsonNumber(row, "\"last_lap_ms\":", 0.0))
                    : 0;
                liveHistory_->setLap(nextLap, nextLapStart, completedLapMs);
                liveLapNum_ = nextLap;
            }
            if (nextLapStart >= 0.0f) liveLapStart_ = nextLapStart;
        }
        if (isStrategyInput && rowTime >= 0.0f) {
            retainedRow = std::make_shared<const std::string>(row);
            strategyRow = {rowTime, ++liveHistorySequence_, retainedRow};
            strategyRows.push_back(strategyRow);
        }
        if (type < liveLatestRows_.size()) liveLatestRows_[type] = row;
        if (rowTime >= 0.0f && type < liveHistoryLastSample_.size()) {
            // A packet inside the rewind grace window is merely late. Do not
            // put it behind newer history: rewind tail-trimming relies on each
            // family remaining time ordered.
            const bool extendsTimeline = rowTime >= liveHistoryLastSample_[type];
            const bool displayHistory = config_.binaryPlayback &&
                (kHistoricalRowMask & (1u << type));
            const int rowLap = type == 4
                ? static_cast<int>(scanJsonNumber(row, "\"lap_num\":", -1))
                : liveLapNum_;
            const bool strategyHistory = isStrategyInput &&
                (type == 6 ||
                 !std::isfinite(liveHistoryLastSample_[type]) ||
                 rowTime >= liveHistoryLastSample_[type] +
                     kStrategyHistoryIntervalS ||
                 (type == 4 && liveHistoryLastLap_[type] != rowLap));
            if (extendsTimeline && (displayHistory || strategyHistory)) {
                if (!retainedRow) {
                    retainedRow = std::make_shared<const std::string>(row);
                    strategyRow = {rowTime, ++liveHistorySequence_, retainedRow};
                }
                liveHistory_->appendJson(type,
                    {strategyRow.sessionTime, strategyRow.sequence, retainedRow});
                liveHistoryLastSample_[type] = rowTime;
                liveHistoryLastLap_[type] = rowLap;
            }
        }
        if (r.rewindSessionTime || type == 0 || (consumerRowMask_ & (1u << type))) emitRow(row);
    }
    if (strategyInput) {
        enqueueLiveStrategyWork({StrategyWorkKind::Update, liveStrategyGeneration_,
                                 liveStrategyFormat_, config_.strategyMinimumStops, false,
                                 std::move(strategyRows)});
    }
    if (config_.binaryPlayback && r.sessionTime >= 0.0f && !r.binary.empty()) {
        (void)bin::forEachPackedRecord(r.binary.data(), r.binary.size(),
            [&](uint8_t type, const uint8_t* record, size_t recordLen) {
                if (!(kHistoricalRowMask & (1u << type))) return;
                liveHistory_->appendPacked(type, r.sessionTime, record, recordLen);
            });
    }
    if (config_.hotRowsAsJson) {
        for (const auto& hj : r.hotJson) {
            const uint8_t type = rowTypeOf(hj);
            if (type == 0 || (consumerRowMask_ & (1u << type))) emitRow(hj);
        }
    } else if (!r.binary.empty()) {
        std::vector<uint8_t> selected;
        selected.reserve(r.binary.size());
        const bool filtered = bin::appendFilteredBatch(
            selected, r.binary.data(), r.binary.size(), consumerRowMask_);
        ++runtimeFilteredBinaryBatches_;
        runtimeFilteredBinaryBytesProduced_ += selected.size();
        runtimeFilteredBinaryCapacityAllocated_ += selected.capacity();
        runtimeLastFilteredBinaryCapacity_ = selected.capacity();
        runtimePeakFilteredBinaryCapacity_ = std::max(
            runtimePeakFilteredBinaryCapacity_, selected.capacity());
        if (filtered && !selected.empty())
            emitBinary(selected.data(), selected.size());
    }
}

void Engine::setOverride(Override ovr) {
    std::string status;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        parser_.setOverride(ovr);
        config_.protocol = ovr;
        status = parser_.statusRow();
    }
    emitRow(status);
}

void Engine::setStrategyMinimumStops(int stops) {
    std::string snapshot;
    bool shouldEmit = false;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        config_.strategyMinimumStops = std::clamp(stops, 0, 8);
        strategy_.setMinimumStops(config_.strategyMinimumStops);
        reader_.setStrategyMinimumStops(config_.strategyMinimumStops);
        if (inPlayback_.load()) {
            if (!liveLatestRows_[kStrategyRowType].empty()) {
                snapshot = strategy_.snapshotJson();
                liveLatestRows_[kStrategyRowType] = snapshot;
                lastStrategyJson_ = snapshot;
                shouldEmit = (consumerRowMask_ & kStrategyRowBit) != 0;
            }
            requestPlaybackStrategyRebuildLocked(currentTime_);
        } else if (!inPlayback_.load()) {
            enqueueLiveStrategyWork({StrategyWorkKind::Configure,
                                     liveStrategyGeneration_, liveStrategyFormat_,
                                     config_.strategyMinimumStops, true, {}});
        }
    }
    if (shouldEmit) emitRow(snapshot);
}

void Engine::setLogging(bool enabled, const std::string& outputDir) {
    setLoggingZstd(enabled, outputDir);
}

void Engine::setLoggingZstd(bool enabled, const std::string& outputDir) {
    std::lock_guard<std::mutex> lk(mutex_);
    config_.loggingEnabled  = enabled;
    config_.outputDirectory = outputDir;
    writer_.setLoggingZstd(enabled, outputDir);
}

void Engine::setLoggingGzip(bool enabled, const std::string& outputDir) {
    std::lock_guard<std::mutex> lk(mutex_);
    config_.loggingEnabled  = enabled;
    config_.outputDirectory = outputDir;
#if defined(__clang__) || defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#elif defined(_MSC_VER)
#  pragma warning(push)
#  pragma warning(disable: 4996)
#endif
    writer_.setLoggingGzip(enabled, outputDir);
#if defined(__clang__) || defined(__GNUC__)
#  pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#  pragma warning(pop)
#endif
}

void Engine::flushRecording() {
    std::lock_guard<std::mutex> lk(mutex_);
    writer_.flushToDisk();
}

void Engine::requestDataRequirements(uint64_t requestId) {
    if (requestId == 0) return;
    uint64_t current = latestRequirementsRequestId_.load(std::memory_order_relaxed);
    while (current < requestId &&
           !latestRequirementsRequestId_.compare_exchange_weak(
               current, requestId, std::memory_order_release,
               std::memory_order_relaxed)) {}
}

void Engine::setDataRequirements(uint32_t streamRowMask,
                                 uint32_t historyRowMask,
                                 float windowSeconds,
                                 uint64_t requestId,
                                 const std::vector<uint8_t>& v6Types,
                                 const std::vector<uint8_t>& v6HistoryTypes) {
    applyDataRequirements(streamRowMask, historyRowMask, windowSeconds, requestId,
                          v6Types, v6HistoryTypes, 0, false);
}

void Engine::applyDataRequirements(uint32_t streamRowMask,
                                   uint32_t historyRowMask,
                                   float windowSeconds,
                                   uint64_t requestId,
                                   const std::vector<uint8_t>& v6Types,
                                   const std::vector<uint8_t>& v6HistoryTypes,
                                   uint32_t forceRestoreMask,
                                   bool pairInitiated) {
    std::vector<std::string> restore;
    float liveBackfillLapStart = 0.0f;
    float liveBackfillStart = 0.0f;
    float liveBackfillThrough = 0.0f;
    int liveBackfillLapNum = 0;
    uint32_t liveBackfillMask = 0;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (requestId != 0 && requestId !=
            latestRequirementsRequestId_.load(std::memory_order_acquire)) return;
        hostConsumerRowMask_ = streamRowMask;
        hostConsumerHistoryMask_ = historyRowMask & streamRowMask;
        hostConsumerWindowSeconds_ = std::max(-1.0f, windowSeconds);
        hostConsumerV6Types_ = v6Types;
        hostConsumerV6HistoryTypes_ = v6HistoryTypes;
        const uint32_t aggregateStreamMask =
            hostConsumerRowMask_ | pairConsumerRowMask_;
        const uint32_t newlyEnabled = aggregateStreamMask & ~consumerRowMask_;
        const uint32_t oldHistoryMask = consumerHistoryMask_;
        const float oldWindowSeconds = consumerWindowSeconds_;
        consumerRowMask_ = aggregateStreamMask;
        // Paired displays need latest state, never chart-history backfills.
        consumerHistoryMask_ = hostConsumerHistoryMask_;
        consumerWindowSeconds_ = hostConsumerWindowSeconds_;
        const uint32_t backfillMask = oldWindowSeconds == consumerWindowSeconds_
            ? consumerHistoryMask_ & ~oldHistoryMask
            : consumerHistoryMask_;
        const uint32_t playbackMask = consumerRowMask_ |
            ((consumerRowMask_ & kStrategyRowBit) ? kStrategyDependencyMask : 0u);
        reader_.setPlaybackRowMask(playbackMask, currentTime_);
        // The reader loads the union of the desktop's and the phones' fields.
        // Re-priming the cursor is not free, so a phone that re-subscribes with
        // a field set the reader already holds leaves it alone. Comparing with
        // the reader's own state (which close() resets) stays correct across
        // file loads. Desktop-initiated calls keep their existing behaviour.
        const std::vector<uint8_t> loadedV6Types = readerV6Types(
            hostConsumerRowMask_, v6Types, pairConsumerV6Types_);
        if (!(pairInitiated && loadedV6Types == reader_.playbackV6Types() &&
              v6HistoryTypes == reader_.playbackV6HistoryTypes()))
            reader_.setPlaybackV6Types(loadedV6Types, v6HistoryTypes, currentTime_);

        // Strategy dependencies are retained while hidden. Materialize one fresh
        // derived row at subscription time so opening the tab never waits for the
        // next UDP packet (or playback tick).
        if (newlyEnabled & kStrategyRowBit) {
            if (inPlayback_.load()) {
                requestPlaybackStrategyRebuildLocked(currentTime_);
            } else {
                enqueueLiveStrategyWork({StrategyWorkKind::Configure,
                                         liveStrategyGeneration_, liveStrategyFormat_,
                                         config_.strategyMinimumStops, true, {}});
            }
        }

        // Historical families are restored by the indexed range request. Only
        // current-state/stream-only families need a latest-row snapshot here.
        // A phone's baseline is its own: it needs the latest value of every
        // family it subscribed to, not just families nobody else had enabled.
        // Telemetry is only restorable from a V6 recording, whose reader can
        // return a field's latest sample; live telemetry is a continuous
        // binary stream and other formats have no JSON latest row for it.
        uint32_t forcedRestore = forceRestoreMask & aggregateStreamMask;
        if (!inPlayback_.load() || reader_.loadedFormat() != TnrdFormat::ChunkedV6)
            forcedRestore &= ~(1u << 1);
        const uint32_t restoreMask =
            (newlyEnabled | forcedRestore) & ~consumerHistoryMask_ & ~kStrategyRowBit;
        if (restoreMask != 0) {
            if (inPlayback_.load()) {
                auto tagged = reader_.latestOfTypesTagged(
                    currentTime_, typesInMask(restoreMask));
                for (auto& [type, row] : tagged) {
                    if (type < dupCache_.size()) dupCache_[type] = row;
                    restore.push_back(std::move(row));
                }
            } else {
                for (size_t type = 1; type < liveLatestRows_.size(); ++type)
                    if ((restoreMask & (1u << type)) &&
                        !liveLatestRows_[type].empty())
                        restore.push_back(liveLatestRows_[type]);
            }
        }

        // The live store owns session history in lap/family segments. A newly
        // visible chart requests only its families; old compressed laps are
        // decompressed by the history worker, never this control/UI thread.
        if (config_.binaryPlayback && !inPlayback_.load() && backfillMask != 0 &&
            liveSessionTime_ > 0.0f) {
            const float fromTime = consumerWindowSeconds_ < 0.0f
                ? 0.0f
                : consumerWindowSeconds_ == 0.0f
                    ? liveLapStart_
                    : std::max(0.0f, liveSessionTime_ - consumerWindowSeconds_);
            liveBackfillLapStart = liveLapStart_;
            liveBackfillStart = fromTime;
            liveBackfillThrough = liveSessionTime_;
            liveBackfillLapNum = liveLapNum_;
            liveBackfillMask = backfillMask;
        }
        appliedRequirementsRequestId_ = std::max(appliedRequirementsRequestId_, requestId);
        if (liveDiagnosticsEnabled_) {
            const std::string streamTypes = byteList(hostConsumerV6Types_);
            const std::string historyTypes = byteList(hostConsumerV6HistoryTypes_);
            std::fprintf(stderr,
                "[playback-debug] requirements-applied request=%llu latest=%llu streamMask=0x%08x historyMask=0x%08x window=%.3f v6Stream=%s v6History=%s\n",
                static_cast<unsigned long long>(requestId),
                static_cast<unsigned long long>(latestRequirementsRequestId_.load(std::memory_order_acquire)),
                hostConsumerRowMask_, hostConsumerHistoryMask_, hostConsumerWindowSeconds_,
                streamTypes.c_str(), historyTypes.c_str());
            std::fflush(stderr);
        }
    }
    requirementsCv_.notify_all();
    emitRows(restore);
    if (sink_ && liveBackfillMask != 0) {
        Sink* const sink = sink_;
        const uint64_t expectedRequestId = requestId;
        liveHistory_->requestRange(
            liveBackfillMask, liveBackfillStart, liveBackfillThrough,
            [this, sink, expectedRequestId, liveBackfillLapStart,
             liveBackfillLapNum, liveBackfillMask, liveBackfillStart]
            (detail::LiveHistoryBackfill backfill) mutable {
                if (expectedRequestId != 0 && expectedRequestId !=
                    latestRequirementsRequestId_.load(std::memory_order_acquire)) return;
                if (!backfill.binary && backfill.json.empty()) return;
                const size_t binarySize = backfill.binary
                    ? backfill.binary->size() : 0;
                sink->onSeekFlush(std::move(backfill.binary), 0, binarySize,
                                  std::move(backfill.json), liveBackfillLapStart,
                                  liveBackfillLapNum, true, 0, false,
                                  liveBackfillMask, liveBackfillStart);
            });
    }
}

void Engine::setTeamColorOverrides(TeamColorOverrides overrides) {
    TeamColorOverrides sanitized = sanitizeTeamColorOverrides(overrides);
    auto snapshot = std::make_shared<const TeamColorOverrides>(sanitized);
    std::atomic_store_explicit(&teamColorOverrides_, snapshot,
                               std::memory_order_release);

    std::string participants;
    std::string strategy;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        config_.teamColorOverrides = std::move(sanitized);
        parser_.setTeamColorOverrides(config_.teamColorOverrides);
        strategy_.setTeamColorOverrides(config_.teamColorOverrides);
        reader_.setTeamColorOverrides(config_.teamColorOverrides);
        participants = inPlayback_.load() ? dupCache_[8] : liveLatestRows_[8];
        if (inPlayback_.load()) {
            if (!liveLatestRows_[kStrategyRowType].empty()) {
                strategy = strategy_.snapshotJson();
                liveLatestRows_[kStrategyRowType] = strategy;
                lastStrategyJson_ = strategy;
            }
            requestPlaybackStrategyRebuildLocked(currentTime_);
        } else if (!inPlayback_.load()) {
            enqueueLiveStrategyWork({StrategyWorkKind::Configure,
                                     liveStrategyGeneration_, liveStrategyFormat_,
                                     config_.strategyMinimumStops, true, {},
                                     liveSessionTime_});
        }
    }
    // Refresh the currently visible roster immediately, including while a
    // recording is paused in playback. Future participant packets/rows use the
    // same resolver automatically.
    if (!participants.empty()) emitRow(participants);
    if (!strategy.empty()) emitRow(strategy);
}

std::string Engine::teamColorCatalogJson() const {
    return tnrp::teamColorCatalogJson();
}

void Engine::setPairDataRequirements(uint32_t streamRowMask,
                                     const std::vector<uint8_t>& v6Types,
                                     bool refreshSnapshot) {
    uint32_t hostStreamMask = 0;
    uint32_t hostHistoryMask = 0;
    float hostWindowSeconds = 0.0f;
    std::vector<uint8_t> hostV6Types;
    std::vector<uint8_t> hostV6HistoryTypes;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        pairConsumerRowMask_ = streamRowMask;
        pairConsumerV6Types_ = v6Types;
        hostStreamMask = hostConsumerRowMask_;
        hostHistoryMask = hostConsumerHistoryMask_;
        hostWindowSeconds = hostConsumerWindowSeconds_;
        hostV6Types = hostConsumerV6Types_;
        hostV6HistoryTypes = hostConsumerV6HistoryTypes_;
    }
    applyDataRequirements(hostStreamMask, hostHistoryMask, hostWindowSeconds, 0,
                          hostV6Types, hostV6HistoryTypes,
                          refreshSnapshot ? streamRowMask : 0, true);
}

// ── Playback ─────────────────────────────────────────────────────────────────

bool Engine::playerLoad(const std::string& path, std::string* errorOut) {
    playbackStrategyGeneration_.fetch_add(1, std::memory_order_release);
    stopPlaybackThread();

    bool ok = false;
    HeaderRow header;
    std::string lapBlocksMsg;
    std::string restrictionMsg;
    std::string statusMsg;
    std::vector<std::string> initState;
    std::vector<std::pair<uint8_t, std::string>> initPanels;
    StrategyWork strategyWork;
    bool queueStrategyWork = false;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        // The selected file may be the recording currently being written.
        // Drain queued rows and make its newest buffered data recoverable before
        // the reader snapshots/decompresses it.
        writer_.flushToDisk();
        reader_.setBinaryPlayback(config_.binaryPlayback);
        reader_.setSparseV6Playback(config_.sparseV6Playback);
        ok = reader_.load(path, header);
        if (!ok && errorOut) *errorOut = reader_.lastError();
        if (ok) {
            ++liveStrategyGeneration_;
            enqueueLiveStrategyWork({StrategyWorkKind::Reset,
                                     liveStrategyGeneration_, liveStrategyFormat_,
                                     config_.strategyMinimumStops, false, {}});
            strategy_.reset();
            strategy_.setFormat(header.protocol >= 2024 ? (uint16_t)header.protocol : 2025);
            emittedFormat_.store(
                header.protocol >= 2024 ? static_cast<uint16_t>(header.protocol) : 2025,
                std::memory_order_release);
            lastStrategyJson_.clear();
            inPlayback_.store(true);
            playing_     = false;
            currentTime_ = reader_.startTime();
            speed_       = 1.0f;
            appliedSeekRequestId_ = latestSeekRequestId_.load(std::memory_order_acquire);
            playbackPath_ = path;
            playbackStrategyPending_ = false;
            playbackStrategyPendingRows_.clear();
            // Playback suspends live ingest. Finalize the live recording on its
            // owner thread so it is complete and a later return to live starts
            // a fresh stream on the next session packet.
            writer_.closeActiveStream();
            liveLatestRows_ = {};
            liveHistory_->reset();
            liveHistoryLastSample_.fill(-std::numeric_limits<float>::infinity());
            liveHistoryLastLap_.fill(-1);
            liveRetirementTimes_.fill(std::numeric_limits<float>::infinity());
            liveHistorySequence_ = 0;
            liveSessionTime_ = 0.0f;
            liveLapStart_ = 0.0f;
            liveLapNum_ = 0;
            lapBlocksMsg = reader_.lapBlocksMessage();
            restrictionMsg = reader_.driverRestrictionMessage(reader_.startTime());
            lastDriverRestriction_ = restrictionMsg;
            if (config_.binaryPlayback) {
                // Label the clip with its recorded format's catalog (the TS
                // glue caches/rebroadcasts protocol_status rows as usual).
                uint16_t fmt = header.protocol >= 2024 ? (uint16_t)header.protocol : 2025;
                statusMsg = Parser::statusRowForFormat(fmt, header.formula);
                // Seed the dup cache and restore the panels the snapshot
                // doesn't cover (status/damage/positions).
                dupCache_ = {};
                initPanels = reader_.latestOfTypesTagged(reader_.startTime(),
                    typesInMask(consumerRowMask_ & kRestoreRowMask));
                for (auto& [tid, line] : initPanels) dupCache_[tid] = line;
            } else {
                initState = reader_.stateSnapshot(reader_.startTime());
            }
            queueStrategyWork = preparePlaybackStrategyRebuildLocked(
                reader_.startTime(), strategyWork);
        } else {
            playbackPath_.clear();
            lastDriverRestriction_.clear();
            playbackStrategyPending_ = false;
            playbackStrategyPendingRows_.clear();
        }
    }

    PlaybackLoadedRow loaded;
    loaded.ok = ok;
    if (ok) loaded.header = header;
    emitRow(writeJsonNullable(loaded));
    if (ok) {
        if (!statusMsg.empty()) emitRow(statusMsg);
        emitRow(lapBlocksMsg);
        if (!restrictionMsg.empty()) emitRow(restrictionMsg);
        for (const auto& row : initState) emitRow(row);
        std::vector<std::string> panelRows;
        panelRows.reserve(initPanels.size());
        for (const auto& [tid, line] : initPanels) panelRows.push_back(line);
        emitRows(panelRows);
        if (queueStrategyWork) enqueueLiveStrategyWork(std::move(strategyWork));
        playRun_.store(true);
        playThread_ = std::thread(&Engine::playbackLoop, this);
        emitPlaybackState();
    }
    return ok;
}

void Engine::playerPlay() {
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (!inPlayback_.load()) return;
        if (currentTime_ >= reader_.totalTime()) {
            // Replay from the top: the cursor has to rewind with the clock, or
            // the drained index never yields another row.
            currentTime_ = reader_.startTime();
            reader_.beginCursorPrime(currentTime_);
            reader_.primeCursor();
            requestPlaybackStrategyRebuildLocked(currentTime_);
        }
        playing_ = true;
    }
    emitPlaybackState();
}

void Engine::playerPause() {
    {
        std::lock_guard<std::mutex> lk(mutex_);
        playing_ = false;
    }
    emitPlaybackState();
}

void Engine::playerRequestSeek(uint64_t requestId) {
    if (requestId == 0) return;
    uint64_t current = latestSeekRequestId_.load(std::memory_order_relaxed);
    while (current < requestId &&
           !latestSeekRequestId_.compare_exchange_weak(
               current, requestId, std::memory_order_release, std::memory_order_relaxed)) {}
}

void Engine::playerSeek(float pct, bool allHistory, uint64_t requestId,
                        uint32_t rowTypeMask, float windowSeconds) {
    std::vector<std::string> state;
    float lapStart = 0.0f;
    int   lapNum   = 0;
    float target   = 0.0f;
    float historyStart = 0.0f;
    TnrdReader::SeekFlush binFlush;
    std::vector<std::pair<uint8_t, std::string>> panels;
    StrategyWork strategyWork;
    bool queueStrategyWork = false;
    std::string seekRestrictionMsg;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (!inPlayback_.load()) return;
        if (requestId != 0 && requestId != latestSeekRequestId_.load(std::memory_order_acquire)) return;
        float start  = reader_.startTime();
        float dur    = std::max(0.0f, reader_.totalTime() - start);
        target = start + std::clamp(pct, 0.0f, 1.0f) * dur;
        lapStart = target;
        reader_.currentLapAt(target, lapStart, lapNum);
        historyStart = allHistory ? start
            : windowSeconds > 0.0f ? std::max(start, target - windowSeconds)
            : lapStart;
        const auto cancelled = [this, requestId] {
            return requestId != 0 && requestId !=
                latestSeekRequestId_.load(std::memory_order_acquire);
        };
        if (config_.binaryPlayback) {
            // Start the V5 target frontier before extracting the prefix. The
            // archive executor can now decompress both sets concurrently, and
            // its in-flight table shares target-containing chunks between them.
            reader_.beginCursorPrime(target);
            binFlush = reader_.seekFlush(target, lapStart, allHistory, rowTypeMask,
                                         windowSeconds, false, cancelled);
            if (cancelled()) return;
            const uint32_t restoreMask = consumerRowMask_ & kRestoreRowMask &
                (~rowTypeMask | kSeekPanelRestoreMask);
            panels = reader_.latestOfTypesTagged(target, typesInMask(restoreMask), cancelled);
            if (cancelled()) return;
        } else {
            state = reader_.stateSnapshot(target, cancelled);
            if (cancelled()) return;
        }

        // A newer request may have arrived while this worker was extracting a
        // large history prefix. An overtaken worker must not move
        // the cursor or seed playback rows from its obsolete target.
        if (requestId != 0 && requestId != latestSeekRequestId_.load(std::memory_order_acquire)) return;
        currentTime_ = target;
        if (config_.binaryPlayback) {
            // beginCursorPrime() already positioned the lanes. This wait is
            // normally a cache/in-flight join because it overlapped the
            // backfill; future chunks remain background-prefetched.
            reader_.primeCursor();
        } else {
            reader_.setCursor(target);
        }
        if (config_.binaryPlayback) {
            dupCache_ = {};
            for (auto& [tid, line] : panels) dupCache_[tid] = line;
        }
        // A seek can cross a restriction change while paused, when no tick
        // follows to notice it.
        if (std::string next = reader_.driverRestrictionMessage(target);
            !next.empty() && next != lastDriverRestriction_) {
            lastDriverRestriction_ = next;
            seekRestrictionMsg = next;
        }
        appliedSeekRequestId_ = requestId;
        queueStrategyWork = preparePlaybackStrategyRebuildLocked(target, strategyWork);
    }

    if (config_.binaryPlayback) {
        const size_t pairLength = binFlush.binaryStore &&
                binFlush.binaryEnd > binFlush.binaryBegin
            ? binFlush.binaryEnd - binFlush.binaryBegin : 0;
        pairServer_.publishSeekSnapshot(
            pairLength > 0
                ? binFlush.binaryStore->data() + binFlush.binaryBegin
                : nullptr,
            pairLength, binFlush.coldJson);
        if (sink_) sink_->onSeekFlush(std::move(binFlush.binaryStore), binFlush.binaryBegin,
                                      binFlush.binaryEnd, std::move(binFlush.coldJson),
                                      lapStart, lapNum, allHistory, requestId, true,
                                      rowTypeMask, historyStart);
        std::vector<std::string> panelRows;
        panelRows.reserve(panels.size());
        for (const auto& [tid, line] : panels) panelRows.push_back(line);
        emitRows(panelRows);
    } else {
        PlaybackSeekFlushRow flush;
        flush.currentLapStart = lapStart;
        flush.lapNum          = lapNum;
        emitRow(writeJson(flush));
        for (const auto& s : state) emitRow(s);
    }
    if (!seekRestrictionMsg.empty()) emitRow(seekRestrictionMsg);
    // Queue only after the authoritative flush has crossed the Sink boundary.
    // Electron will then buffer an early Strategy result behind its
    // waiting-renderer barrier instead of discarding it as pre-seek state.
    if (queueStrategyWork) enqueueLiveStrategyWork(std::move(strategyWork));
    emitPlaybackState();
}

void Engine::playerSetSpeed(float mult) {
    {
        std::lock_guard<std::mutex> lk(mutex_);
        speed_ = mult;
    }
    emitPlaybackState();
}

void Engine::playerSetDriver(int driverIndex, bool useRecordedRows) {
    std::string lapBlocks;
    std::string restriction;
    std::vector<std::pair<uint8_t, std::string>> panels;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (!inPlayback_.load()) return;
        reader_.setPlaybackDriver(driverIndex, useRecordedRows, currentTime_);
        lapBlocks = reader_.lapBlocksMessage();
        // Restated unconditionally on a driver change: a paired client has no
        // other way to learn that the new driver's private data is withheld.
        restriction = reader_.driverRestrictionMessage(currentTime_);
        lastDriverRestriction_ = restriction;
        panels = reader_.latestOfTypesTagged(currentTime_,
            typesInMask(consumerRowMask_ & kRestoreRowMask));
        dupCache_ = {};
        for (const auto& [type, row] : panels)
            if (type < dupCache_.size()) dupCache_[type] = row;
    }
    emitRow(lapBlocks);
    if (!restriction.empty()) emitRow(restriction);
    std::vector<std::string> panelRows;
    panelRows.reserve(panels.size());
    for (const auto& panel : panels) panelRows.push_back(panel.second);
    emitRows(panelRows);
}

void Engine::liveGetFastestLap(uint64_t requestId) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (inPlayback_.load()) return;
    liveHistory_->requestFastestLap(
        [this, requestId](int lap, int ms, float start, float end,
                          detail::LiveHistoryBackfill data) {
            std::string msg = "{\"type\":\"live_fastest_lap_data\",\"requestId\":" +
                std::to_string(requestId) + ",\"lapNum\":" + std::to_string(lap) +
                ",\"lapTimeMs\":" + std::to_string(ms) +
                ",\"startSessionTime\":" + std::to_string(start) +
                ",\"endSessionTime\":" + std::to_string(end) + ",\"binary\":[";
            if (data.binary) {
                for (size_t i = 0; i < data.binary->size(); ++i) {
                    if (i) msg += ',';
                    msg += std::to_string((*data.binary)[i]);
                }
            }
            msg += "],\"rows\":[";
            // Stored JSON is newline-delimited, with no trailing newline.
            std::replace(data.json.begin(), data.json.end(), '\n', ',');
            msg += data.json;
            msg += "]}";
            emitRow(msg);
        });
}

void Engine::playerGetLapData(int lapNum, uint32_t rowTypeMask) {
    std::string msg;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (!inPlayback_.load()) return;
        msg = reader_.getLapDataMessage(lapNum, rowTypeMask);
    }
    if (!msg.empty()) emitRow(msg);
}

std::string Engine::playerGetAnalysisLapData(int lapNum, uint32_t rowTypeMask,
                                             int driverIndex) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return inPlayback_.load()
        ? reader_.getLapDataMessage(lapNum, rowTypeMask, driverIndex)
        : std::string{};
}

bool Engine::playerGetAnalysisLapProgress(int lapNum, AnalysisLapProgress& out,
                                          int driverIndex) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return inPlayback_.load() && reader_.getAnalysisLapProgress(lapNum, out, driverIndex);
}

void Engine::playerGetAllLapsData(uint64_t requestId, uint32_t rowTypeMask) {
    float lapStart = 0.0f;
    int lapNum = 0;
    TnrdReader::SeekFlush flush;
    float historyStart = 0.0f;
    {
        std::unique_lock<std::mutex> lk(mutex_);
        requirementsCv_.wait(lk, [this] {
            return !inPlayback_.load() ||
                appliedRequirementsRequestId_ >= latestRequirementsRequestId_.load(std::memory_order_acquire);
        });
        if (!inPlayback_.load() || !config_.binaryPlayback) return;
        const float target = currentTime_;
        lapStart = target;
        reader_.currentLapAt(target, lapStart, lapNum);
        if (liveDiagnosticsEnabled_) {
            const std::string historyTypes = byteList(hostConsumerV6HistoryTypes_);
            std::fprintf(stderr,
                "[playback-debug] history-read-start mode=all request=%llu requirements=%llu target=%.3f lapStart=%.3f rowMask=0x%08x v6History=%s\n",
                static_cast<unsigned long long>(requestId),
                static_cast<unsigned long long>(appliedRequirementsRequestId_),
                target, lapStart, rowTypeMask, historyTypes.c_str());
            std::fflush(stderr);
        }
        flush = reader_.seekFlush(target, lapStart, true, rowTypeMask);
        historyStart = reader_.startTime();
        if (liveDiagnosticsEnabled_) {
            const size_t binaryBytes = flush.binaryStore && flush.binaryEnd > flush.binaryBegin
                ? flush.binaryEnd - flush.binaryBegin : 0;
            std::fprintf(stderr,
                "[playback-debug] history-read-finish mode=all request=%llu binary=%zu json=%zu historyStart=%.3f\n",
                static_cast<unsigned long long>(requestId), binaryBytes,
                flush.coldJson.size(), historyStart);
            std::fflush(stderr);
        }
    }
    if (sink_) sink_->onSeekFlush(std::move(flush.binaryStore), flush.binaryBegin,
                                  flush.binaryEnd, std::move(flush.coldJson),
                                  lapStart, lapNum, true, requestId, false,
                                  rowTypeMask, historyStart);
}

void Engine::playerGetWindowData(float windowSeconds, uint64_t requestId,
                                 uint32_t rowTypeMask) {
    float lapStart = 0.0f;
    int lapNum = 0;
    TnrdReader::SeekFlush flush;
    float historyStart = 0.0f;
    {
        std::unique_lock<std::mutex> lk(mutex_);
        requirementsCv_.wait(lk, [this] {
            return !inPlayback_.load() ||
                appliedRequirementsRequestId_ >= latestRequirementsRequestId_.load(std::memory_order_acquire);
        });
        if (!inPlayback_.load() || !config_.binaryPlayback || windowSeconds < 0.0f) return;
        const float target = currentTime_;
        lapStart = target;
        reader_.currentLapAt(target, lapStart, lapNum);
        if (liveDiagnosticsEnabled_) {
            const std::string historyTypes = byteList(hostConsumerV6HistoryTypes_);
            std::fprintf(stderr,
                "[playback-debug] history-read-start mode=window request=%llu requirements=%llu target=%.3f lapStart=%.3f window=%.3f rowMask=0x%08x v6History=%s\n",
                static_cast<unsigned long long>(requestId),
                static_cast<unsigned long long>(appliedRequirementsRequestId_),
                target, lapStart, windowSeconds, rowTypeMask, historyTypes.c_str());
            std::fflush(stderr);
        }
        flush = reader_.seekFlush(target, lapStart, false, rowTypeMask, windowSeconds,
                                  false);
        historyStart = windowSeconds > 0.0f
            ? std::max(reader_.startTime(), target - windowSeconds)
            : lapStart;
        if (liveDiagnosticsEnabled_) {
            const size_t binaryBytes = flush.binaryStore && flush.binaryEnd > flush.binaryBegin
                ? flush.binaryEnd - flush.binaryBegin : 0;
            std::fprintf(stderr,
                "[playback-debug] history-read-finish mode=window request=%llu binary=%zu json=%zu historyStart=%.3f\n",
                static_cast<unsigned long long>(requestId), binaryBytes,
                flush.coldJson.size(), historyStart);
            std::fflush(stderr);
        }
    }
    // Finite-window backfill is additive at the renderer just like an AL family
    // request; it does not move the playhead or replace newer buffered rows.
    if (sink_) sink_->onSeekFlush(std::move(flush.binaryStore), flush.binaryBegin,
                                  flush.binaryEnd, std::move(flush.coldJson),
                                  lapStart, lapNum, true, requestId, false,
                                  rowTypeMask, historyStart);
}

void Engine::playerClose() {
    std::fprintf(stderr, "[close-trace] Engine::playerClose entry; stopping playback thread\n");
    std::fflush(stderr);
    // Cancel any off-thread reconstruction before invalidating the playback
    // timeline. The strategy worker owns a separate reader, so cancellation is
    // non-blocking and does not delay closing the main playback reader.
    playbackStrategyGeneration_.fetch_add(1, std::memory_order_release);
    stopPlaybackThread();
    std::fprintf(stderr, "[close-trace] playback thread stopped; waiting for engine mutex\n");
    std::fflush(stderr);
    std::string liveStatus;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        std::fprintf(stderr, "[close-trace] engine mutex acquired; closing reader\n");
        std::fflush(stderr);
        reader_.close();
        std::fprintf(stderr, "[close-trace] reader close returned\n");
        std::fflush(stderr);
        inPlayback_.store(false);
        emittedFormat_.store(parser_.activeFormat(), std::memory_order_release);
        playing_ = false;
        appliedSeekRequestId_ = latestSeekRequestId_.load(std::memory_order_acquire);
        playbackStrategyPending_ = false;
        playbackStrategyPendingRows_.clear();
        playbackPath_.clear();
        if (config_.binaryPlayback) {
            dupCache_ = {};
            // Restore the live format's labels — playback may have switched
            // them (e.g. a 2026 clip while the live game is 2025). Before any
            // live packet was seen the parser falls back to the 2025 catalog.
            liveStatus = parser_.statusRow();
        }
        strategy_.reset();
        ++liveStrategyGeneration_;
        enqueueLiveStrategyWork({StrategyWorkKind::Reset,
                                 liveStrategyGeneration_, liveStrategyFormat_,
                                 config_.strategyMinimumStops, false, {}});
        lastStrategyJson_.clear();
    }
    requirementsCv_.notify_all();
    emitRow(writeJson(TypeOnlyRow{"playback_close"}));
    if (!liveStatus.empty()) emitRow(liveStatus);
    std::fprintf(stderr, "[close-trace] Engine::playerClose complete\n");
    std::fflush(stderr);
}

void Engine::stopPlaybackThread() {
    playRun_.store(false);
    if (playThread_.joinable()) {
        std::fprintf(stderr, "[close-trace] joining playback thread\n");
        std::fflush(stderr);
        playThread_.join();
        std::fprintf(stderr, "[close-trace] playback thread joined\n");
        std::fflush(stderr);
    }
}

void Engine::emitPlaybackState() {
    std::string st;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        float start = reader_.startTime();
        PlaybackStateRow s;
        s.playing      = playing_;
        s.current_time = currentTime_ - start;
        s.total_time   = std::max(0.0f, reader_.totalTime() - start);
        s.speed        = speed_;
        s.start_time   = start;
        st = writeJson(s);
    }
    emitRow(st);
}

void Engine::playbackLoop() {
    using clock = std::chrono::steady_clock;
    auto last = clock::now();

    // Reused across ticks to avoid a per-tick allocation churn in binary mode.
    std::string          jsonBatch;
    std::vector<uint8_t> binBatch;

    while (playRun_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
        auto now = clock::now();
        double dt = std::chrono::duration<double>(now - last).count();
        last = now;

        std::vector<std::string> batch;
        jsonBatch.clear();
        binBatch.clear();
        std::string strategyMsg;
        std::string restrictionMsg;
        uint32_t emitMask = 0;
        uint64_t tickSeekRequestId = 0;
        bool finished = false;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            if (!playing_) continue;
            if (latestSeekRequestId_.load(std::memory_order_acquire) != appliedSeekRequestId_) continue;
            tickSeekRequestId = appliedSeekRequestId_;
            double step = dt * speed_;
            if (step > 0.10) step = 0.10;
            currentTime_ += (float)step;

            float total = reader_.totalTime();
            bool  atEnd = currentTime_ >= total;
            if (atEnd) currentTime_ = total;

            // The setting can change mid-session, so the cursor can cross a
            // change without a driver switch. Silent unless the row differs.
            if (std::string next = reader_.driverRestrictionMessage(currentTime_);
                !next.empty() && next != lastDriverRestriction_) {
                lastDriverRestriction_ = next;
                restrictionMsg = std::move(next);
            }

            if (config_.binaryPlayback) {
                uint32_t seen = 0;
                reader_.pullUntilSplit(atEnd ? INFINITY : currentTime_,
                                       jsonBatch, binBatch, seen, &dupCache_);
                // Re-emit the sparse panel rows that delivered nothing this
                // tick, session_time moved to the playhead (only on ticks that
                // delivered something — a fully idle tick stays silent).
                if (seen != 0) {
                    for (uint8_t tid : kDupTypeIds) {
                        if (!(consumerRowMask_ & (1u << tid))) continue;
                        if ((seen & (1u << tid)) || dupCache_[tid].empty()) continue;
                        std::string dup = dupCache_[tid];
                        setSessionTime(dup, currentTime_);
                        jsonBatch += dup;
                        jsonBatch.push_back('\n');
                    }
                }
            } else if (atEnd) {
                batch = reader_.drainRest();
            } else {
                batch = reader_.pullUntil(currentTime_);
            }

            // While an asynchronous seek reconstruction is running, retain its
            // subsequent dependency rows instead of applying them to the stale
            // pre-seek reducer. The worker folds this catch-up tail into its
            // result atomically before committing it.
            bool strategyUpdated = false;
            for (const auto& row : batch) {
                const uint8_t type = rowTypeOf(row);
                if (kStrategyDependencyMask & (1u << type)) {
                    if (playbackStrategyPending_)
                        playbackStrategyPendingRows_.push_back(row);
                    else {
                        ingestStrategyRow(row);
                        strategyUpdated = true;
                    }
                }
            }
            size_t strategyStart = 0;
            while (strategyStart < jsonBatch.size()) {
                size_t nl = jsonBatch.find('\n', strategyStart);
                if (nl == std::string::npos) nl = jsonBatch.size();
                if (nl > strategyStart) {
                    std::string_view row(jsonBatch.data() + strategyStart,
                                         nl - strategyStart);
                    const uint8_t type = rowTypeOf(row);
                    if (kStrategyDependencyMask & (1u << type)) {
                        if (playbackStrategyPending_)
                            playbackStrategyPendingRows_.emplace_back(row);
                        else {
                            strategy_.ingestJson(row);
                            strategyUpdated = true;
                        }
                    }
                }
                strategyStart = nl + 1;
            }
            if (strategyUpdated) {
                std::string row = strategy_.snapshotJson();
                liveLatestRows_[kStrategyRowType] = row;
                if ((consumerRowMask_ & kStrategyRowBit) && row != lastStrategyJson_)
                    strategyMsg = row;
                lastStrategyJson_ = std::move(row);
            }
            emitMask = consumerRowMask_;

            if (atEnd) {
                playing_ = false;
                finished = true;
            }
        }
        // A request registered after this tick was decoded owns the renderer
        // from here on. Drop the old batch and let its seek flush replace it.
        if (latestSeekRequestId_.load(std::memory_order_acquire) != tickSeekRequestId)
            continue;
        std::vector<std::string> outputRows;
        outputRows.reserve(batch.size());
        for (const auto& row : batch) {
            const uint8_t type = rowTypeOf(row);
            if (type == 0 || (emitMask & (1u << type))) outputRows.push_back(row);
        }
        if (!jsonBatch.empty()) {
            size_t start = 0;
            while (start < jsonBatch.size()) {
                size_t nl = jsonBatch.find('\n', start);
                if (nl == std::string::npos) nl = jsonBatch.size();
                if (nl > start) {
                    std::string row = jsonBatch.substr(start, nl - start);
                    const uint8_t type = rowTypeOf(row);
                    if (type == 0 || (emitMask & (1u << type)))
                        outputRows.push_back(std::move(row));
                }
                start = nl + 1;
            }
        }
        emitRows(outputRows);
        if (!restrictionMsg.empty()) emitRow(restrictionMsg);
        if (!strategyMsg.empty()) emitRow(strategyMsg);
        if (!binBatch.empty()) emitBinary(binBatch.data(), binBatch.size());
        emitPlaybackState();
        if (finished) emitRow(writeJson(TypeOnlyRow{"playback_finished"}));
    }
}

} // namespace tnrp
