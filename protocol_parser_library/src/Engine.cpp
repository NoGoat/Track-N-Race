#include "tnrp/Engine.h"
#include "tnrp/TimeUtils.h"
#include "tnrp/control_rows.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

#define TRACE(msg) do { fprintf(stderr, "[native] " msg "\n"); fflush(stderr); } while (0)

namespace tnrp {

// Sparse panel-row type ids re-emitted each binary-playback tick so their
// panels track the playhead between native ~2 Hz updates (reader ids:
// status, damage, lap, session, timing, all_status, positions).
static constexpr uint8_t kDupTypeIds[] = { 2, 3, 4, 5, 7, 9, 13 };

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
    : config_(config), sink_(sink), parser_(config.protocol) {
    TRACE("Engine ctor: start");
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

void Engine::restartUdp(uint16_t port, const std::string& bindAddress) {
    udp_.stop();
    {
        std::lock_guard<std::mutex> lk(mutex_);
        config_.port        = port;
        config_.bindAddress = bindAddress;
        parser_.reset();
    }
    udp_.start(port, bindAddress, [this](const uint8_t* d, int n) { onDatagram(d, n); });
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
    Parser::Result r = parser_.feed(data, length, ts, wantHotJson);

    for (const auto& c : r.control) emitRow(c);
    if (r.dropped) return;

    if (recording) {
        writer_.notePacket(r.format, r.packetId, r.sessionTime, data, length);
        for (const auto& row : r.rows)    writer_.record(row, r.sessionTime);
        for (const auto& hj  : r.hotJson) writer_.record(hj, r.sessionTime);
    }

    // Cold rows always go to the live JSON channel. Hot rows go either to the
    // live binary channel (default) or, for an in-process JSON-only consumer, to
    // the JSON channel as well — mutually exclusive so a sink sees each hot row once.
    for (const auto& row : r.rows) emitRow(row);
    if (config_.hotRowsAsJson) {
        for (const auto& hj : r.hotJson) emitRow(hj);
    } else if (!r.binary.empty() && sink_) {
        sink_->onBinary(r.binary.data(), r.binary.size());
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

// ── Playback ─────────────────────────────────────────────────────────────────

bool Engine::playerLoad(const std::string& path) {
    stopPlaybackThread();

    bool ok = false;
    HeaderRow header;
    std::string lapBlocksMsg;
    std::string statusMsg;
    std::vector<std::string> initState;
    std::vector<std::pair<uint8_t, std::string>> initPanels;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        reader_.setBinaryPlayback(config_.binaryPlayback);
        ok = reader_.load(path, header);
        if (ok) {
            inPlayback_.store(true);
            playing_     = false;
            currentTime_ = reader_.startTime();
            speed_       = 1.0f;
            writer_.closeActiveStream();
            lapBlocksMsg = reader_.lapBlocksMessage();
            initState = reader_.stateSnapshot(reader_.startTime());
            if (config_.binaryPlayback) {
                // Label the clip with its recorded format's catalog (the TS
                // glue caches/rebroadcasts protocol_status rows as usual).
                uint16_t fmt = header.protocol >= 2024 ? (uint16_t)header.protocol : 2025;
                statusMsg = Parser::statusRowForFormat(fmt);
                // Seed the dup cache and restore the panels the snapshot
                // doesn't cover (status/damage/positions).
                dupCache_ = {};
                initPanels = reader_.latestOfTypesTagged(
                    reader_.startTime(), { std::begin(kDupTypeIds), std::end(kDupTypeIds) });
                for (auto& [tid, line] : initPanels) dupCache_[tid] = line;
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
        for (const auto& s : initState) emitRow(s);
        for (const auto& [tid, line] : initPanels)
            if (tid == 2 || tid == 3 || tid == 13) emitRow(line);
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

void Engine::playerSeek(float pct) {
    std::vector<std::string> state;
    float lapStart = 0.0f;
    int   lapNum   = 0;
    float target   = 0.0f;
    TnrdReader::SeekFlush binFlush;
    std::vector<std::pair<uint8_t, std::string>> panels;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (!inPlayback_.load()) return;
        float start  = reader_.startTime();
        float dur    = std::max(0.0f, reader_.totalTime() - start);
        target = start + std::clamp(pct, 0.0f, 1.0f) * dur;
        currentTime_ = target;
        reader_.setCursor(target);
        state    = reader_.stateSnapshot(target);
        lapStart = target;
        reader_.currentLapAt(target, lapStart, lapNum);
        if (config_.binaryPlayback) {
            binFlush = reader_.seekFlush(target, lapStart);
            // Re-key the dup cache to the seek point so the next tick's
            // re-emissions carry the right panel data.
            dupCache_ = {};
            panels = reader_.latestOfTypesTagged(
                target, { std::begin(kDupTypeIds), std::end(kDupTypeIds) });
            for (auto& [tid, line] : panels) dupCache_[tid] = line;
        }
    }

    if (config_.binaryPlayback) {
        if (sink_) sink_->onSeekFlush(binFlush.binary.data(), binFlush.binary.size(),
                                      binFlush.coldJson, lapStart, lapNum);
        for (const auto& s : state) emitRow(s);
        // status/damage already ride in the flush's coldJson; positions has no
        // cold cache, so restore the track map explicitly.
        for (const auto& [tid, line] : panels)
            if (tid == 13) emitRow(line);
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

void Engine::playerGetLapData(int lapNum) {
    std::string msg;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (!inPlayback_.load()) return;
        msg = reader_.getLapDataMessage(lapNum);
    }
    if (!msg.empty()) emitRow(msg);
}

void Engine::playerClose() {
    stopPlaybackThread();
    std::string liveStatus;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        reader_.close();
        inPlayback_.store(false);
        playing_ = false;
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
