#include "tnrp/Engine.h"
#include "tnrp/TimeUtils.h"

#include <algorithm>
#include <chrono>
#include <thread>

namespace tnrp {

Engine::Engine(const Config& config, Sink* sink)
    : config_(config), sink_(sink), parser_(config.protocol) {
    writer_.setLogging(config.loggingEnabled, config.outputDirectory);
    // Surface the initial protocol state so the UI reflects override/last-detected
    // even before any packets arrive.
    emitRow(parser_.statusRow());
}

Engine::~Engine() {
    stopPlaybackThread();
    udp_.stop();
    writer_.closeActiveStream();
}

void Engine::emitRow(const nlohmann::json& row) {
    if (sink_) sink_->onRow(row);
}

// ── Live ─────────────────────────────────────────────────────────────────────

bool Engine::startUdp() {
    return udp_.start(config_.port, config_.bindAddress,
                      [this](const uint8_t* d, int n) { onDatagram(d, n); });
}

void Engine::restartUdp(uint16_t port, const std::string& bindAddress) {
    // Stop the receive thread WITHOUT holding mutex_: a stop() that joined while
    // an in-flight onDatagram waited on mutex_ would deadlock.
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
    if (inPlayback_.load()) return;  // playback owns the output; drop live packets

    std::lock_guard<std::mutex> lk(mutex_);
    std::string ts = isoTimestamp();
    Parser::Result r = parser_.feed(data, length, ts);

    for (const auto& c : r.control) emitRow(c);
    if (r.dropped) return;

    writer_.notePacket(r.format, r.packetId, r.sessionTime, data, length);
    for (const auto& row : r.rows) {
        writer_.record(row, r.sessionTime);
        emitRow(row);
    }
}

void Engine::setOverride(Override ovr) {
    nlohmann::json status;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        parser_.setOverride(ovr);
        config_.protocol = ovr;
        status = parser_.statusRow();
    }
    emitRow(status);
}

void Engine::setLogging(bool enabled, const std::string& outputDir) {
    std::lock_guard<std::mutex> lk(mutex_);
    config_.loggingEnabled  = enabled;
    config_.outputDirectory = outputDir;
    writer_.setLogging(enabled, outputDir);
}

// ── Playback ─────────────────────────────────────────────────────────────────

bool Engine::playerLoad(const std::string& path) {
    stopPlaybackThread();

    bool ok = false;
    nlohmann::json header;
    nlohmann::json lapBlocks;
    std::vector<nlohmann::json> initState;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        ok = reader_.load(path, header);
        if (ok) {
            inPlayback_.store(true);
            playing_     = false;
            currentTime_ = reader_.startTime();
            speed_       = 1.0f;
            writer_.closeActiveStream();   // never record during playback
            // Built once on load: per-lap Speed/RPM/ERS blocks + lap list + events.
            // This is what flips the renderer into true playback mode (so it stops
            // re-filtering a growing buffer every frame), plus the initial panel state.
            lapBlocks = reader_.lapBlocksMessage();
            lapBlocks["type"] = "playback_lap_blocks";
            initState = reader_.stateSnapshot(reader_.startTime());
        }
    }

    emitRow({ {"type", "playback_loaded"}, {"ok", ok}, {"header", ok ? header : nlohmann::json(nullptr)} });
    if (ok) {
        emitRow(lapBlocks);
        for (const auto& s : initState) emitRow(s);
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
        if (currentTime_ >= reader_.totalTime()) currentTime_ = reader_.startTime();  // replay from start
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
    std::vector<nlohmann::json> state;
    float lapStart = 0.0f;
    int   lapNum   = 0;
    float target   = 0.0f;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (!inPlayback_.load()) return;
        float start  = reader_.startTime();
        float dur    = std::max(0.0f, reader_.totalTime() - start);
        target = start + std::clamp(pct, 0.0f, 1.0f) * dur;
        currentTime_ = target;
        reader_.setCursor(target);                 // play resumes forward from here
        state    = reader_.stateSnapshot(target);  // panel-state setters
        lapStart = target;
        reader_.currentLapAt(target, lapStart, lapNum);
    }

    emitRow({
        {"type", "playback_seek_flush"},
        {"currentLapStart", lapStart}, {"lapNum", lapNum},
    });
    for (const auto& s : state) emitRow(s);
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
    nlohmann::json lapData;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (!inPlayback_.load()) return;
        lapData = reader_.getLapDataMessage(lapNum);
    }
    if (!lapData.is_null()) {
        lapData["type"] = "playback_lap_data";
        emitRow(lapData);
    }
}

void Engine::playerClose() {
    stopPlaybackThread();
    {
        std::lock_guard<std::mutex> lk(mutex_);
        reader_.close();
        inPlayback_.store(false);
        playing_ = false;
    }
    emitRow({ {"type", "playback_close"} });
}

void Engine::stopPlaybackThread() {
    playRun_.store(false);
    if (playThread_.joinable()) playThread_.join();
}

void Engine::emitPlaybackState() {
    nlohmann::json st;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        float start = reader_.startTime();
        st = {
            {"type",       "playback_state"},
            {"playing",    playing_},
            {"current_time", currentTime_ - start},
            {"total_time",   std::max(0.0f, reader_.totalTime() - start)},
            {"speed",      speed_},
        };
    }
    emitRow(st);
}

void Engine::playbackLoop() {
    using clock = std::chrono::steady_clock;
    auto last = clock::now();

    while (playRun_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
        auto now = clock::now();
        double dt = std::chrono::duration<double>(now - last).count();
        last = now;

        std::vector<nlohmann::json> batch;
        bool finished = false;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            if (!playing_) continue;   // paused: idle but keep the thread alive
            double step = dt * speed_;
            if (step > 0.10) step = 0.10;   // cap per-tick advance (graceful under stall)
            currentTime_ += (float)step;

            float total = reader_.totalTime();
            if (currentTime_ >= total) {
                currentTime_ = total;
                batch = reader_.drainRest();
                playing_ = false;
                finished = true;
            } else {
                batch = reader_.pullUntil(currentTime_);
            }
        }
        for (const auto& row : batch) emitRow(row);
        emitPlaybackState();
        if (finished) emitRow({ {"type", "playback_finished"} });
    }
}

} // namespace tnrp
