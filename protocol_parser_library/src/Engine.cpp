#include "tnrp/Engine.h"
#include "tnrp/BinaryRows.h"
#include "tnrp/TimeUtils.h"
#include "tnrp/control_rows.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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
static constexpr uint32_t kRestoreRowMask =
    (1u << 2) | (1u << 3) | (1u << 4) | (1u << 5) | (1u << 7) |
    (1u << 8) | (1u << 9) | (1u << 10) | (1u << 13) | (1u << 14);
static constexpr uint32_t kHistoricalRowMask =
    (1u << 1) | (1u << 2) | (1u << 3) | (1u << 4) | (1u << 11) |
    (1u << 12);
static constexpr size_t kMaxLiveHistoryRows = 750000;

static uint8_t rowTypeOf(std::string_view json) {
    static constexpr std::pair<std::string_view, uint8_t> TYPES[] = {
        {"\"type\":\"telemetry\"", 1}, {"\"type\":\"status\"", 2},
        {"\"type\":\"damage\"", 3}, {"\"type\":\"lap\"", 4},
        {"\"type\":\"session\"", 5}, {"\"type\":\"race_event\"", 6},
        {"\"type\":\"timing\"", 7}, {"\"type\":\"participants\"", 8},
        {"\"type\":\"all_status\"", 9}, {"\"type\":\"tyre_sets\"", 10},
        {"\"type\":\"motion\"", 11}, {"\"type\":\"motion_ex\"", 12},
        {"\"type\":\"positions\"", 13},
        {"\"type\":\"session_history_fastest\"", 14},
    };
    for (const auto& [needle, type] : TYPES)
        if (json.find(needle) != std::string_view::npos) return type;
    return 0;
}

static std::vector<uint8_t> typesInMask(uint32_t mask) {
    std::vector<uint8_t> out;
    for (uint8_t type = 1; type < 16; ++type)
        if (mask & (1u << type)) out.push_back(type);
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

// Emits a newline-terminated multi-row batch through the per-row Sink::onRow
// contract.
static void emitLines(Sink* sink, const std::string& batch) {
    if (!sink) return;
    size_t start = 0;
    while (start < batch.size()) {
        size_t nl = batch.find('\n', start);
        if (nl == std::string::npos) nl = batch.size();
        if (nl > start) sink->onRow(batch.substr(start, nl - start));
        start = nl + 1;
    }
}

Engine::Engine(const Config& config, Sink* sink)
    : config_(config), sink_(sink), parser_(config.protocol),
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
    // Electron declares its visible consumers immediately after renderer
    // mount. Start that host closed so no telemetry can slip through before
    // the first aggregate subscription; JSON-only/Qt hosts keep legacy-all.
    if (config_.binaryPlayback) consumerRowMask_ = 0;
    writer_.setLogging(config.loggingEnabled, config.outputDirectory);
    TRACE("Engine ctor: writer_.setLogging done");
    emitRow(parser_.statusRow());
    TRACE("Engine ctor: emitRow(statusRow) done");
}

Engine::~Engine() {
    stopPlaybackThread();
    udp_.stop();
    writer_.closeActiveStream();
}

void Engine::emitRow(const std::string& json) {
    if (sink_) sink_->onRow(json);
}

// ── Live ─────────────────────────────────────────────────────────────────────

bool Engine::startUdp() {
    TRACE("Engine::startUdp: start");
    bool ok = udp_.start(config_.port, config_.bindAddress,
                      [this](const uint8_t* d, int n) { onDatagram(d, n); });
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
    }
    return udp_.start(port, bindAddress,
                      [this](const uint8_t* d, int n) { onDatagram(d, n); });
}

std::string Engine::udpLastError() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return udp_.lastError();
}

void Engine::onDatagram(const uint8_t* data, int length) {
    if (inPlayback_.load()) return;

    std::lock_guard<std::mutex> lk(mutex_);
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
    const uint32_t parserMask = recording ? 0xFFFFFFFFu : consumerRowMask_ |
        (config_.binaryPlayback ? kHistoricalRowMask : 0u);
    Parser::Result r = parser_.feed(data, length, ts, wantHotJson, parserMask);

    for (const auto& c : r.control) emitRow(c);
    if (r.dropped) return;

    if (recording) {
        writer_.notePacket(r.format, r.packetId, r.sessionTime, data, length);
        for (const auto& row : r.rows)    writer_.record(row, r.sessionTime);
        for (const auto& hj  : r.hotJson) writer_.record(hj, r.sessionTime);
    }

    if (config_.binaryPlayback && std::isfinite(r.sessionTime) && r.sessionTime >= 0.0f &&
        r.sessionTime < liveSessionTime_) {
        for (auto& history : livePackedHistory_) {
            while (!history.index.empty() &&
                   history.index.back().sessionTime > r.sessionTime)
                history.index.pop_back();
            const size_t keep = history.index.empty() ? 0 :
                history.index.back().offset + history.index.back().length;
            if (keep < history.bytes.size()) history.bytes.resize(keep);
            history.discardedBytes = std::min(history.discardedBytes, keep);
        }
        for (auto& history : liveJsonHistory_)
            while (!history.empty() && history.back().sessionTime > r.sessionTime)
                history.pop_back();
    }
    if (config_.binaryPlayback && std::isfinite(r.sessionTime) && r.sessionTime >= 0.0f)
        liveSessionTime_ = r.sessionTime;

    // Keep one native latest-state row even while its page is hidden, then only
    // forward subscribed families. Recording above remains completely unmasked.
    for (const auto& row : r.rows) {
        const uint8_t type = rowTypeOf(row);
        if (type < liveLatestRows_.size()) liveLatestRows_[type] = row;
        if (config_.binaryPlayback && r.sessionTime >= 0.0f && type < liveJsonHistory_.size() &&
            (kHistoricalRowMask & (1u << type))) {
            auto& history = liveJsonHistory_[type];
            history.push_back({r.sessionTime, row});
            if (history.size() > kMaxLiveHistoryRows) history.pop_front();
        }
        if (config_.binaryPlayback && type == 4) {
            liveLapNum_ = static_cast<int>(scanJsonNumber(
                row, "\"lap_num\":", liveLapNum_));
            const double lapMs = scanJsonNumber(row, "\"current_lap_ms\":", 0.0);
            if (r.sessionTime >= 0.0f && lapMs >= 0.0)
                liveLapStart_ = r.sessionTime - static_cast<float>(lapMs / 1000.0);
        }
        if (type == 0 || (consumerRowMask_ & (1u << type))) emitRow(row);
    }
    if (config_.binaryPlayback && r.sessionTime >= 0.0f && !r.binary.empty()) {
        (void)bin::forEachPackedRecord(r.binary.data(), r.binary.size(),
            [&](uint8_t type, const uint8_t* record, size_t recordLen) {
                if (!(kHistoricalRowMask & (1u << type))) return;
                auto& history = livePackedHistory_[type];
                const size_t offset = history.bytes.size();
                history.bytes.insert(history.bytes.end(), record, record + recordLen);
                history.index.push_back({r.sessionTime, offset, recordLen});
                if (history.index.size() > kMaxLiveHistoryRows) {
                    const auto& old = history.index.front();
                    history.discardedBytes = old.offset + old.length;
                    history.index.pop_front();
                }
                if (history.discardedBytes >= 8u * 1024u * 1024u &&
                    history.discardedBytes * 2 >= history.bytes.size()) {
                    const size_t discarded = history.discardedBytes;
                    history.bytes.erase(history.bytes.begin(),
                        history.bytes.begin() + static_cast<std::ptrdiff_t>(discarded));
                    for (auto& entry : history.index) entry.offset -= discarded;
                    history.discardedBytes = 0;
                }
            });
    }
    if (config_.hotRowsAsJson) {
        for (const auto& hj : r.hotJson) {
            const uint8_t type = rowTypeOf(hj);
            if (type == 0 || (consumerRowMask_ & (1u << type))) emitRow(hj);
        }
    } else if (!r.binary.empty() && sink_) {
        std::vector<uint8_t> selected;
        selected.reserve(r.binary.size());
        if (bin::appendFilteredBatch(selected, r.binary.data(), r.binary.size(),
                                     consumerRowMask_) && !selected.empty())
            sink_->onBinary(selected.data(), selected.size());
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
                                 uint64_t requestId) {
    std::vector<std::string> restore;
    std::shared_ptr<std::vector<uint8_t>> liveBackfillBinary;
    std::string liveBackfillJson;
    float liveBackfillLapStart = 0.0f;
    int liveBackfillLapNum = 0;
    uint32_t liveBackfillMask = 0;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (requestId != 0 && requestId !=
            latestRequirementsRequestId_.load(std::memory_order_acquire)) return;
        const uint32_t newlyEnabled = streamRowMask & ~consumerRowMask_;
        const uint32_t oldHistoryMask = consumerHistoryMask_;
        const float oldWindowSeconds = consumerWindowSeconds_;
        consumerRowMask_ = streamRowMask;
        consumerHistoryMask_ = historyRowMask & streamRowMask;
        consumerWindowSeconds_ = std::max(-1.0f, windowSeconds);
        const uint32_t backfillMask = oldWindowSeconds == consumerWindowSeconds_
            ? consumerHistoryMask_ & ~oldHistoryMask
            : consumerHistoryMask_;
        reader_.setPlaybackRowMask(consumerRowMask_, currentTime_);

        // Historical families are restored by the indexed range request. Only
        // current-state/stream-only families need a latest-row snapshot here.
        const uint32_t restoreMask = newlyEnabled & ~consumerHistoryMask_;
        if (restoreMask != 0) {
            if (inPlayback_.load()) {
                auto tagged = reader_.latestOfTypesTagged(
                    currentTime_, typesInMask(restoreMask));
                for (auto& [type, row] : tagged) {
                    if (type < dupCache_.size()) dupCache_[type] = row;
                    restore.push_back(std::move(row));
                }
            } else {
                for (uint8_t type = 1; type < liveLatestRows_.size(); ++type)
                    if ((restoreMask & (1u << type)) &&
                        !liveLatestRows_[type].empty())
                        restore.push_back(liveLatestRows_[type]);
            }
        }

        // Live mode keeps raw bounded history natively. Showing a previously
        // hidden chart backfills packed/JSON rows without having processed them
        // in Electron while the consumer was invisible.
        if (config_.binaryPlayback && !inPlayback_.load() && backfillMask != 0 &&
            liveSessionTime_ > 0.0f) {
            const float fromTime = consumerWindowSeconds_ < 0.0f
                ? 0.0f
                : consumerWindowSeconds_ == 0.0f
                    ? liveLapStart_
                    : std::max(0.0f, liveSessionTime_ - consumerWindowSeconds_);
            liveBackfillBinary = std::make_shared<std::vector<uint8_t>>();
            for (uint8_t type = 1; type < 16; ++type) {
                if (!(backfillMask & (1u << type))) continue;
                for (const auto& entry : livePackedHistory_[type].index) {
                    if (entry.sessionTime < fromTime ||
                        entry.sessionTime > liveSessionTime_) continue;
                    const auto& bytes = livePackedHistory_[type].bytes;
                    if (entry.offset + entry.length > bytes.size()) continue;
                    liveBackfillBinary->insert(liveBackfillBinary->end(),
                        bytes.begin() + static_cast<std::ptrdiff_t>(entry.offset),
                        bytes.begin() + static_cast<std::ptrdiff_t>(entry.offset + entry.length));
                }
                for (const auto& entry : liveJsonHistory_[type]) {
                    if (entry.sessionTime < fromTime ||
                        entry.sessionTime > liveSessionTime_) continue;
                    liveBackfillJson += entry.json;
                    liveBackfillJson.push_back('\n');
                }
            }
            if (!liveBackfillJson.empty()) liveBackfillJson.pop_back();
            liveBackfillLapStart = liveLapStart_;
            liveBackfillLapNum = liveLapNum_;
            liveBackfillMask = backfillMask;
            if (liveBackfillBinary->empty() && liveBackfillJson.empty())
                liveBackfillBinary.reset();
        }
    }
    for (const auto& row : restore) emitRow(row);
    if (sink_ && (liveBackfillBinary || !liveBackfillJson.empty())) {
        const size_t binarySize = liveBackfillBinary
            ? liveBackfillBinary->size() : 0;
        sink_->onSeekFlush(liveBackfillBinary, 0, binarySize,
                           std::move(liveBackfillJson), liveBackfillLapStart,
                           liveBackfillLapNum, true, 0, false,
                           liveBackfillMask);
    }
}

// ── Playback ─────────────────────────────────────────────────────────────────

bool Engine::playerLoad(const std::string& path, std::string* errorOut) {
    stopPlaybackThread();

    bool ok = false;
    HeaderRow header;
    std::string lapBlocksMsg;
    std::string statusMsg;
    std::vector<std::string> initState;
    std::vector<std::pair<uint8_t, std::string>> initPanels;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        // The selected file may be the recording currently being written.
        // Drain queued rows and make its newest buffered data recoverable before
        // the reader snapshots/decompresses it.
        writer_.flushToDisk();
        reader_.setBinaryPlayback(config_.binaryPlayback);
        ok = reader_.load(path, header);
        if (!ok && errorOut) *errorOut = reader_.lastError();
        if (ok) {
            inPlayback_.store(true);
            playing_     = false;
            currentTime_ = reader_.startTime();
            speed_       = 1.0f;
            appliedSeekRequestId_ = latestSeekRequestId_.load(std::memory_order_acquire);
            // Playback suspends live ingest. Finalize the live recording on its
            // owner thread so it is complete and a later return to live starts
            // a fresh stream on the next session packet.
            writer_.closeActiveStream();
            liveLatestRows_ = {};
            for (auto& history : livePackedHistory_) history = {};
            for (auto& history : liveJsonHistory_) history.clear();
            liveSessionTime_ = 0.0f;
            liveLapStart_ = 0.0f;
            liveLapNum_ = 0;
            lapBlocksMsg = reader_.lapBlocksMessage();
            if (config_.binaryPlayback) {
                // Label the clip with its recorded format's catalog (the TS
                // glue caches/rebroadcasts protocol_status rows as usual).
                uint16_t fmt = header.protocol >= 2024 ? (uint16_t)header.protocol : 2025;
                statusMsg = Parser::statusRowForFormat(fmt);
                // Seed the dup cache and restore the panels the snapshot
                // doesn't cover (status/damage/positions).
                dupCache_ = {};
                initPanels = reader_.latestOfTypesTagged(reader_.startTime(),
                    typesInMask(consumerRowMask_ & kRestoreRowMask));
                for (auto& [tid, line] : initPanels) dupCache_[tid] = line;
            } else {
                initState = reader_.stateSnapshot(reader_.startTime());
            }
        }
    }

    PlaybackLoadedRow loaded;
    loaded.ok = ok;
    if (ok) loaded.header = header;
    emitRow(writeJsonNullable(loaded));
    if (ok) {
        if (!statusMsg.empty()) emitRow(statusMsg);
        emitRow(lapBlocksMsg);
        for (const auto& row : initState) emitRow(row);
        for (const auto& [tid, line] : initPanels) emitRow(line);
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
            reader_.setCursor(currentTime_);
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
    TnrdReader::SeekFlush binFlush;
    std::vector<std::pair<uint8_t, std::string>> panels;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (!inPlayback_.load()) return;
        if (requestId != 0 && requestId != latestSeekRequestId_.load(std::memory_order_acquire)) return;
        float start  = reader_.startTime();
        float dur    = std::max(0.0f, reader_.totalTime() - start);
        target = start + std::clamp(pct, 0.0f, 1.0f) * dur;
        lapStart = target;
        reader_.currentLapAt(target, lapStart, lapNum);
        if (config_.binaryPlayback) {
            binFlush = reader_.seekFlush(target, lapStart, allHistory, rowTypeMask,
                                         windowSeconds, false);
            const uint32_t restoreMask = consumerRowMask_ & kRestoreRowMask &
                (~rowTypeMask | kDupRowMask);
            panels = reader_.latestOfTypesTagged(target, typesInMask(restoreMask));
        } else {
            state = reader_.stateSnapshot(target);
        }
        // A newer request may have arrived while this worker was extracting a
        // large AL prefix. An overtaken worker must not move the cursor or seed
        // playback rows from its obsolete target.
        if (requestId != 0 && requestId != latestSeekRequestId_.load(std::memory_order_acquire)) return;
        currentTime_ = target;
        reader_.setCursor(target);
        if (config_.binaryPlayback) {
            dupCache_ = {};
            for (auto& [tid, line] : panels) dupCache_[tid] = line;
        }
        appliedSeekRequestId_ = requestId;
    }

    if (config_.binaryPlayback) {
        if (sink_) sink_->onSeekFlush(std::move(binFlush.binaryStore), binFlush.binaryBegin,
                                      binFlush.binaryEnd, std::move(binFlush.coldJson),
                                      lapStart, lapNum, allHistory, requestId, true,
                                      rowTypeMask);
        for (const auto& [tid, line] : panels) emitRow(line);
    } else {
        PlaybackSeekFlushRow flush;
        flush.currentLapStart = lapStart;
        flush.lapNum          = lapNum;
        emitRow(writeJson(flush));
        for (const auto& s : state) emitRow(s);
    }
    emitPlaybackState();
}

void Engine::playerSetSpeed(float mult) {
    {
        std::lock_guard<std::mutex> lk(mutex_);
        speed_ = mult;
    }
    emitPlaybackState();
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

void Engine::playerGetAllLapsData(uint64_t requestId, uint32_t rowTypeMask) {
    float lapStart = 0.0f;
    int lapNum = 0;
    TnrdReader::SeekFlush flush;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (!inPlayback_.load() || !config_.binaryPlayback) return;
        const float target = currentTime_;
        lapStart = target;
        reader_.currentLapAt(target, lapStart, lapNum);
        flush = reader_.seekFlush(target, lapStart, true, rowTypeMask);
    }
    if (sink_) sink_->onSeekFlush(std::move(flush.binaryStore), flush.binaryBegin,
                                  flush.binaryEnd, std::move(flush.coldJson),
                                  lapStart, lapNum, true, requestId, false,
                                  rowTypeMask);
}

void Engine::playerGetWindowData(float windowSeconds, uint64_t requestId,
                                 uint32_t rowTypeMask) {
    float lapStart = 0.0f;
    int lapNum = 0;
    TnrdReader::SeekFlush flush;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (!inPlayback_.load() || !config_.binaryPlayback || windowSeconds < 0.0f) return;
        const float target = currentTime_;
        lapStart = target;
        reader_.currentLapAt(target, lapStart, lapNum);
        flush = reader_.seekFlush(target, lapStart, false, rowTypeMask, windowSeconds,
                                  false);
    }
    // Finite-window backfill is additive at the renderer just like an AL family
    // request; it does not move the playhead or replace newer buffered rows.
    if (sink_) sink_->onSeekFlush(std::move(flush.binaryStore), flush.binaryBegin,
                                  flush.binaryEnd, std::move(flush.coldJson),
                                  lapStart, lapNum, true, requestId, false,
                                  rowTypeMask);
}

void Engine::playerClose() {
    stopPlaybackThread();
    std::string liveStatus;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        reader_.close();
        inPlayback_.store(false);
        playing_ = false;
        appliedSeekRequestId_ = latestSeekRequestId_.load(std::memory_order_acquire);
        if (config_.binaryPlayback) {
            dupCache_ = {};
            // Restore the live format's labels — playback may have switched
            // them (e.g. a 2026 clip while the live game is 2025). Before any
            // live packet was seen the parser falls back to the 2025 catalog.
            liveStatus = parser_.statusRow();
        }
    }
    emitRow(writeJson(TypeOnlyRow{"playback_close"}));
    if (!liveStatus.empty()) emitRow(liveStatus);
}

void Engine::stopPlaybackThread() {
    playRun_.store(false);
    if (playThread_.joinable()) playThread_.join();
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
        bool finished = false;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            if (!playing_) continue;
            if (latestSeekRequestId_.load(std::memory_order_acquire) != appliedSeekRequestId_) continue;
            double step = dt * speed_;
            if (step > 0.10) step = 0.10;
            currentTime_ += (float)step;

            float total = reader_.totalTime();
            bool  atEnd = currentTime_ >= total;
            if (atEnd) currentTime_ = total;

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

            if (atEnd) {
                playing_ = false;
                finished = true;
            }
        }
        for (const auto& row : batch) emitRow(row);
        if (!jsonBatch.empty()) emitLines(sink_, jsonBatch);
        if (!binBatch.empty() && sink_) sink_->onBinary(binBatch.data(), binBatch.size());
        emitPlaybackState();
        if (finished) emitRow(writeJson(TypeOnlyRow{"playback_finished"}));
    }
}

} // namespace tnrp
