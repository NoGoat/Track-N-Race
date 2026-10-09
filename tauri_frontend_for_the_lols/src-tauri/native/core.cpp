#include "core.h"
#include <tnrp/Engine.h>
#include <tnrp/LapDelta.h>
#include <tnrp/XlsxExport.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <stdexcept>

namespace tnr_tauri {
struct Command {
    std::string op, path, destination, format = "auto", bindAddress = "0.0.0.0";
    std::string outputDirectory, pairName = "Track N Race Tauri", pairStateJson, id;
    int port = 20777, pairPort = 20779, driverIndex = -1, lapNum = 0;
    int comparisonLap = 0, comparisonDriver = -1, strategyMinimumStops = 1;
    bool enabled = false, pairEnabled = false, loggingEnabled = false;
    bool allHistory = false, secondary = false, comparisonSecondary = false;
    bool sectorDelta = false, useRecordedRows = false, visible = true;
    double value = 0, windowSeconds = 0;
    uint64_t requestId = 0, sequence = 0;
    uint32_t rowTypeMask = 0xFFFFFFFFu, streamMask = 0xFFFFFFFFu, historyMask = 0;
    std::vector<uint8_t> v6Types, v6HistoryTypes;
    std::vector<tnrp::UdpForwardTarget> forwardTargets;
    tnrp::TeamColorOverrides teamColorOverrides;
};
struct Result { bool ok = true; std::string error; glz::raw_json data{"null"}; };
struct Restore { float chartFrom{}; bool includesEvents{}; float eventsFrom{}, through{}; bool sessionChanged{}; };
struct Flush {
    std::string type = "playback_seek_flush_bin", coldJson;
    float currentLapStart{}; int lapNum{}; bool allHistory{};
    uint64_t requestId{}; bool authoritativeSeek = true;
    uint32_t rowTypeMask = 0xFFFFFFFFu; float historyStart{};
    std::optional<Restore> restore;
};
struct Envelope { std::string kind; glz::raw_json payload{"null"}; };
struct PairState { glz::raw_json state; std::string persisted; };
struct ExportProgress { double pct; std::string stage; };
struct AnalysisLoad { glz::raw_json data; int trackId; std::string trackName; };
struct Event {
    std::string kind, payload;
    std::vector<uint8_t> binary;
    bool expendable = false;
    uint64_t request = 0;
    bool authoritative = false;
    size_t size() const { return payload.size() + binary.size(); }
};

// Commands are serialized by Rust. next() has one independent consumer.
// Callback memory is copied before returning to the engine.
class Host final : public tnrp::Sink {
public:
    std::mutex mutex;
    std::condition_variable ready;
    std::deque<Event> events;
    size_t queuedBytes = 0;
    bool gap = false;
    std::atomic<bool> visible{true};
    tnrp::TnrdReader analysis;
    // Destroy the engine and join its producers before destroying the queue.
    std::unique_ptr<tnrp::Engine> engine;

    void push(Event e) {
        std::lock_guard lock(mutex);
        constexpr size_t limit = 32 * 1024 * 1024;
        if (e.expendable && !visible.load()) return;
        // Never block the recording thread on a hidden or slow webview.
        // A gap requests authoritative restoration, rather than silent loss.
        while (queuedBytes + e.size() > limit) {
            auto it = std::find_if(events.begin(), events.end(), [](const auto& v) { return v.expendable; });
            if (it == events.end()) break;
            queuedBytes -= it->size(); events.erase(it); gap = true;
        }
        if (e.expendable && queuedBytes + e.size() > limit) { gap = true; return; }
        if (!events.empty() && e.expendable && events.back().expendable &&
            events.back().kind == e.kind && events.back().size() + e.size() < 1024 * 1024) {
            auto& back = events.back(); queuedBytes += e.size();
            if (e.kind == "json") back.payload += e.payload;
            back.binary.insert(back.binary.end(), e.binary.begin(), e.binary.end());
            // Binary payload is the constant "null", not concatenated.
            if (e.kind == "binary") queuedBytes -= e.payload.size();
        } else { queuedBytes += e.size(); events.push_back(std::move(e)); }
        ready.notify_one();
    }
    void onRow(const std::string& json) override {
        const bool critical = json.find("\"type\":\"protocol_status\"") != std::string::npos ||
            json.find("\"type\":\"recording_error\"") != std::string::npos ||
            json.find("\"type\":\"playback_lap_blocks\"") != std::string::npos ||
            json.find("\"type\":\"playback_lap_data\"") != std::string::npos ||
            json.find("\"type\":\"playback_loaded\"") != std::string::npos ||
            json.find("\"type\":\"playback_close\"") != std::string::npos;
        push({"json", json + "\n", {}, !critical});
    }
    void onRows(const std::vector<std::string>& rows) override { for (const auto& row : rows) onRow(row); }
    void onBinary(const uint8_t* data, size_t len) override {
        if (len && visible.load()) push({"binary", "null", {data, data + len}, true});
    }
    void flush(std::shared_ptr<const std::vector<uint8_t>> store, size_t begin, size_t end, Flush meta) {
        std::vector<uint8_t> bytes;
        if (store && end > begin) {
            if (end > store->size()) throw std::runtime_error("Invalid library history slice");
            bytes.assign(store->begin() + begin, store->begin() + end);
        }
        push({"flush", tnrp::writeJson(meta), std::move(bytes), false, meta.requestId, meta.authoritativeSeek});
    }
    void onSeekFlush(std::shared_ptr<const std::vector<uint8_t>> store, size_t begin, size_t end,
        std::string&& json, float lapStart, int lap, bool all, uint64_t request,
        bool authoritative, uint32_t mask, float historyStart) override {
        Flush f; f.coldJson = std::move(json); f.currentLapStart = lapStart; f.lapNum = lap;
        f.allHistory = all; f.requestId = request; f.authoritativeSeek = authoritative;
        f.rowTypeMask = mask; f.historyStart = historyStart;
        flush(std::move(store), begin, end, std::move(f));
    }
    void onRestoreFlush(std::shared_ptr<const std::vector<uint8_t>> store, size_t begin, size_t end,
        std::string&& json, const RestoreFlushInfo& info) override {
        Flush f; f.coldJson = std::move(json); f.currentLapStart = info.currentLapStart;
        f.lapNum = info.lapNum; f.rowTypeMask = info.rowTypeMask; f.historyStart = info.chartFrom;
        f.authoritativeSeek = false;
        f.restore = Restore{info.chartFrom, info.includesEvents, info.eventsFrom, info.through, info.sessionChanged};
        flush(std::move(store), begin, end, std::move(f));
    }
    void onPairState(const std::string& state, const std::string& persisted) override {
        push({"pair", tnrp::writeJson(PairState{glz::raw_json{state}, persisted}), {}});
    }
    void onPairDiagnostic(const std::string& message) override {
        push({"diagnostic", tnrp::writeJson(message), {}});
    }
    void liveLap(std::string header, std::shared_ptr<std::vector<uint8_t>> history) {
        push({"live-lap", tnrp::writeJson(header), history ? *history : std::vector<uint8_t>{}});
    }
    Result call(const Command& c) {
        if (c.op == "initialize") {
            if (engine) { engine->flushRecording(); engine.reset(); }
            { std::lock_guard lock(mutex); events.clear(); queuedBytes = 0; gap = false; }
            if (c.port < 1 || c.port > 65535 || c.pairPort < 1 || c.pairPort > 65535)
                throw std::runtime_error("Port must be between 1 and 65535");
            tnrp::Config config;
            config.port = static_cast<uint16_t>(c.port); config.bindAddress = c.bindAddress;
            config.protocol = tnrp::overrideFromString(c.format);
            config.udpForwardTargets = c.forwardTargets;
            config.teamColorOverrides = tnrp::sanitizeTeamColorOverrides(c.teamColorOverrides);
            config.strategyMinimumStops = std::clamp(c.strategyMinimumStops, 0, 8);
            config.binaryPlayback = config.sparseV6Playback = config.columnarV6History = true;
            config.pairEnabled = c.pairEnabled; config.pairPort = static_cast<uint16_t>(c.pairPort);
            config.pairName = c.pairName; config.pairStateJson = c.pairStateJson;
            config.loggingEnabled = c.loggingEnabled; config.outputDirectory = c.outputDirectory;
            engine = std::make_unique<tnrp::Engine>(config, this);
            engine->setDiagnosticsEnabled(c.enabled);
            const bool ok = engine->startUdp();
            return {true, "", glz::raw_json{tnrp::writeJson(Result{ok, ok ? "" : engine->udpLastError()})}};
        }
        if (c.op == "shutdown") {
            std::string persisted = engine ? engine->pairPersistedStateJson() : "null";
            if (engine) { engine->flushRecording(); engine.reset(); }
            analysis.close(); return {true, "", glz::raw_json{persisted}};
        }
        if (!engine) throw std::runtime_error("Telemetry engine is not initialized");
        auto& e = *engine;
        if (c.op == "play") e.playerPlay();
        else if (c.op == "pause") e.playerPause();
        else if (c.op == "speed") e.playerSetSpeed(static_cast<float>(c.value));
        else if (c.op == "load") { std::string error; bool ok = e.playerLoad(c.path, &error); return {ok, error}; }
        else if (c.op == "close") e.playerClose();
        else if (c.op == "seek") {
            {
                std::lock_guard lock(mutex);
                for (auto it = events.begin(); it != events.end();) {
                    if (it->kind == "flush" && it->request != 0 && it->request < c.requestId) {
                        queuedBytes -= it->size(); it = events.erase(it);
                    } else ++it;
                }
            }
            e.playerRequestSeek(c.requestId);
            e.playerSeek(static_cast<float>(std::clamp(c.value, 0.0, 1.0)), c.allHistory, c.requestId,
                         c.rowTypeMask, static_cast<float>(std::max(0.0, c.windowSeconds)));
        }
        else if (c.op == "driver") e.playerSetDriver(c.driverIndex, c.useRecordedRows);
        else if (c.op == "focus-driver") e.playerSetFocusDriver(c.driverIndex);
        else if (c.op == "lap-history-car") e.setLapHistoryCar(c.driverIndex);
        else if (c.op == "lap") e.playerGetLapData(c.lapNum, c.rowTypeMask);
        else if (c.op == "all-laps") e.playerGetAllLapsData(c.requestId, c.rowTypeMask);
        else if (c.op == "window") e.playerGetWindowData(static_cast<float>(c.windowSeconds), c.requestId, c.rowTypeMask);
        else if (c.op == "requirements") {
            e.requestDataRequirements(c.requestId);
            e.setDataRequirements(c.streamMask, c.historyMask, static_cast<float>(c.windowSeconds),
                                 c.requestId, c.v6Types, c.v6HistoryTypes);
        }
        else if (c.op == "visibility") { visible.store(c.visible); e.setHostVisible(c.visible, c.sequence); }
        else if (c.op == "live-fastest") e.liveGetFastestLap(c.requestId, [this](auto h, auto b) { liveLap(std::move(h), std::move(b)); });
        else if (c.op == "live-lap") e.liveGetLapData(c.requestId, c.lapNum, [this](auto h, auto b) { liveLap(std::move(h), std::move(b)); });
        else if (c.op == "protocol") e.setOverride(tnrp::overrideFromString(c.format));
        else if (c.op == "team-colors") e.setTeamColorOverrides(tnrp::sanitizeTeamColorOverrides(c.teamColorOverrides));
        else if (c.op == "team-catalog") return {true, "", glz::raw_json{e.teamColorCatalogJson()}};
        else if (c.op == "strategy") e.setStrategyMinimumStops(std::clamp(c.strategyMinimumStops, 0, 8));
        else if (c.op == "logging") e.setLogging(c.enabled, c.outputDirectory);
        else if (c.op == "diagnostics") e.setDiagnosticsEnabled(c.enabled);
        else if (c.op == "flush") e.flushRecording();
        else if (c.op == "pair-state") return {true, "", glz::raw_json{e.pairStateJson()}};
        else if (c.op == "pair-start") { std::string error; e.pairStart(&error); return {true, "", glz::raw_json{e.pairStateJson()}}; }
        else if (c.op == "pair-stop") { e.pairStop(); return {true, "", glz::raw_json{e.pairStateJson()}}; }
        else if (c.op == "pair-open") { e.pairOpenWindow(); return {true, "", glz::raw_json{e.pairStateJson()}}; }
        else if (c.op == "pair-close") { e.pairCloseWindow(); return {true, "", glz::raw_json{e.pairStateJson()}}; }
        else if (c.op == "pair-remove") { e.pairRemoveDevice(c.id); return {true, "", glz::raw_json{e.pairStateJson()}}; }
        else if (c.op == "analysis-load") {
            tnrp::HeaderRow header; analysis.setLapStatusSummaries(true);
            if (!analysis.load(c.path, header)) return {false, analysis.lastError()};
            return {true, "", glz::raw_json{tnrp::writeJson(AnalysisLoad{glz::raw_json{analysis.lapBlocksMessage()}, header.track_id, header.track_name})}};
        }
        else if (c.op == "analysis-lap") {
            auto json = c.secondary ? analysis.getLapDataMessage(c.lapNum, c.rowTypeMask, c.driverIndex)
                                    : e.playerGetAnalysisLapData(c.lapNum, c.rowTypeMask, c.driverIndex);
            return {true, "", glz::raw_json{json.empty() ? "null" : json}};
        }
        else if (c.op == "analysis-compare") {
            tnrp::AnalysisLapProgress a, b;
            bool haveA = c.secondary ? analysis.getAnalysisLapProgress(c.lapNum, a, c.driverIndex)
                                    : e.playerGetAnalysisLapProgress(c.lapNum, a, c.driverIndex);
            bool haveB = c.comparisonSecondary ? analysis.getAnalysisLapProgress(c.comparisonLap, b, c.comparisonDriver)
                                               : e.playerGetAnalysisLapProgress(c.comparisonLap, b, c.comparisonDriver);
            if (!haveA || !haveB) return {};
            return {true, "", glz::raw_json{tnrp::writeJson(tnrp::calculateLapDelta(a, b, c.sectorDelta))}};
        }
        else if (c.op == "analysis-close") analysis.close();
        else if (c.op == "export") {
            std::string error;
            bool ok = tnrp::exportTnrdFileToXlsx(c.path, c.destination, &error,
                [this](size_t done, size_t total, const std::string& stage) {
                    push({"export", tnrp::writeJson(ExportProgress{total ? 100.0 * done / total : 100.0, stage}), {}});
                });
            return {ok, error};
        }
        else throw std::runtime_error("Unknown native command: " + c.op);
        return {};
    }
};
char* copyString(const std::string& value) {
    auto* p = static_cast<char*>(std::malloc(value.size() + 1));
    if (p) std::memcpy(p, value.c_str(), value.size() + 1);
    return p;
}
}
using namespace tnr_tauri;
extern "C" void* tnr_create() { try { return new Host; } catch (...) { return nullptr; } }
extern "C" char* tnr_call(void* host, const char* json) {
    try {
        if (!host || !json) throw std::runtime_error("Invalid native command");
        Command c;
        if (auto error = glz::read_json(c, std::string_view{json}); error)
            throw std::runtime_error(glz::format_error(error, std::string_view{json}));
        return copyString(tnrp::writeJson(static_cast<Host*>(host)->call(c)));
    } catch (const std::exception& e) { return copyString(tnrp::writeJson(Result{false, e.what()})); }
      catch (...) { return copyString(tnrp::writeJson(Result{false, "Unknown native exception"})); }
}
extern "C" uint8_t* tnr_next(void* host, size_t* length) {
    *length = 0;
    try {
        auto& h = *static_cast<Host*>(host);
        Event event;
        {
            std::unique_lock lock(h.mutex);
            h.ready.wait_for(lock, std::chrono::milliseconds(100), [&] { return h.gap || !h.events.empty(); });
            if (h.gap) { event = {"gap", "null", {}}; h.gap = false; }
            else if (h.events.empty()) return nullptr;
            else { event = std::move(h.events.front()); h.events.pop_front(); h.queuedBytes -= event.size(); }
        }
        auto payload = event.kind == "json" ? tnrp::writeJson(event.payload) : event.payload;
        auto header = tnrp::writeJson(Envelope{event.kind, glz::raw_json{payload}});
        if (header.size() > std::numeric_limits<uint32_t>::max()) throw std::runtime_error("Event header too large");
        const size_t size = 4 + header.size() + event.binary.size();
        auto* p = static_cast<uint8_t*>(std::malloc(size));
        if (!p) return nullptr;
        for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(header.size() >> (8 * i));
        std::memcpy(p + 4, header.data(), header.size());
        if (!event.binary.empty()) std::memcpy(p + 4 + header.size(), event.binary.data(), event.binary.size());
        *length = size; return p;
    } catch (...) { return nullptr; }
}
extern "C" void tnr_free(void* p) { std::free(p); }
extern "C" void tnr_destroy(void* host) { delete static_cast<Host*>(host); }
