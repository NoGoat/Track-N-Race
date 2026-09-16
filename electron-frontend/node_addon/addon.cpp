#include <napi.h>
#include <tnrp/Engine.h>
#include <tnrp/Labels.h>
#include <tnrp/CardColors.h>
#include <tnrp/LapDelta.h>
#include <tnrp/TnrdReader.h>
#include <tnrp/TeamColors.h>
#include <tnrp/XlsxExport.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using namespace Napi;

#define TRACE(msg) do { fprintf(stderr, "[native] " msg "\n"); fflush(stderr); } while (0)

template <typename T>
static void updateAtomicMaximum(std::atomic<T>& target, T value) {
    T current = target.load(std::memory_order_relaxed);
    while (current < value && !target.compare_exchange_weak(
        current, value, std::memory_order_relaxed, std::memory_order_relaxed)) {}
}

static tnrp::TeamColorOverrides readTeamColorOverrides(const Napi::Value& value) {
    tnrp::TeamColorOverrides result;
    if (!value.IsObject()) return result;
    Napi::Object formats = value.As<Napi::Object>();
    for (const uint16_t format : {2024, 2025, 2026}) {
        const std::string formatKey = std::to_string(format);
        Napi::Value teamsValue = formats.Get(formatKey);
        if (!teamsValue.IsObject()) continue;
        Napi::Object teams = teamsValue.As<Napi::Object>();
        Napi::Array ids = teams.GetPropertyNames();
        for (uint32_t i = 0; i < ids.Length(); ++i) {
            Napi::Value idValue = ids.Get(i);
            if (!idValue.IsString()) continue;
            const std::string idText = idValue.As<Napi::String>().Utf8Value();
            char* end = nullptr;
            const unsigned long parsed = std::strtoul(idText.c_str(), &end, 10);
            if (!end || *end != '\0' || parsed > 65535) continue;
            Napi::Value color = teams.Get(idValue);
            if (!color.IsString()) continue;
            result[format][static_cast<uint16_t>(parsed)] =
                color.As<Napi::String>().Utf8Value();
        }
    }
    return tnrp::sanitizeTeamColorOverrides(result);
}

// Runs the (potentially several-second) XLSX export off the JS thread via
// libuv's threadpool, resolving a Promise on completion. Progress (0..100)
// is reported back through AsyncProgressQueueWorker's queue, which safely
// marshals each Send() from the worker thread onto the JS thread as an
// OnProgress() call — that's where the JS progress callback is invoked.
// One progress tick carried across the threadpool→JS boundary: a percentage
// plus the human-readable stage message describing what the export is doing.
struct ExportTick {
    double pct;
    std::string stage;
};

class ExportXlsxWorker : public Napi::AsyncProgressQueueWorker<ExportTick> {
public:
    ExportXlsxWorker(Napi::Env env, std::string src, std::string dest, Napi::Function progressCb)
        : Napi::AsyncProgressQueueWorker<ExportTick>(env), src_(std::move(src)), dest_(std::move(dest)),
          progressCb_(Napi::Persistent(progressCb)),
          deferred_(Napi::Promise::Deferred::New(env)) {}

    void Execute(const ExecutionProgress& progress) override {   // libuv worker thread — no Napi/JS access here
        ok_ = tnrp::exportTnrdFileToXlsx(src_, dest_, &error_,
            [&progress](size_t done, size_t total, const std::string& stage) {
                ExportTick tick;
                tick.pct = total > 0 ? (100.0 * static_cast<double>(done) / static_cast<double>(total)) : 100.0;
                tick.stage = stage;
                progress.Send(&tick, 1);
            });
    }
    void OnProgress(const ExportTick* data, size_t count) override {   // back on the JS thread
        if (count == 0 || progressCb_.IsEmpty()) return;
        Napi::HandleScope scope(Env());
        const ExportTick& tick = data[count - 1];
        progressCb_.Call({ Napi::Number::New(Env(), tick.pct), Napi::String::New(Env(), tick.stage) });
    }
    void OnOK() override {
        Napi::HandleScope scope(Env());
        Napi::Object result = Napi::Object::New(Env());
        result.Set("ok", ok_);
        if (!ok_) result.Set("error", error_);
        deferred_.Resolve(result);
    }
    void OnError(const Napi::Error& e) override { deferred_.Reject(e.Value()); }
    Napi::Promise GetPromise() { return deferred_.Promise(); }

private:
    std::string src_, dest_, error_;
    bool ok_ = false;
    Napi::FunctionReference progressCb_;
    Napi::Promise::Deferred deferred_;
};

// Runs Engine::playerLoad (gzip decompress + full index scan — seconds for a
// long session) off the JS thread so the Electron main process stays
// responsive during the load, resolving Promise<{ok, error?}>. The engine is held
// by shared_ptr so a destroy() racing a pending load can't free it out from
// under the worker; `busy` serializes loads (a second concurrent load resolves
// with an explanatory failure instead of racing stopPlaybackThread).
class PlayerLoadWorker : public Napi::AsyncWorker {
public:
    PlayerLoadWorker(Napi::Env env, std::shared_ptr<tnrp::Engine> engine,
                     std::string path, std::shared_ptr<std::atomic<bool>> busy)
        : Napi::AsyncWorker(env), engine_(std::move(engine)), path_(std::move(path)),
          busy_(std::move(busy)), deferred_(Napi::Promise::Deferred::New(env)) {}

    void Execute() override {   // libuv worker thread — no Napi/JS access here
        ok_ = engine_->playerLoad(path_, &error_);
    }
    void OnOK() override {
        busy_->store(false);
        Napi::Object result = Napi::Object::New(Env());
        result.Set("ok", ok_);
        if (!ok_) result.Set("error", error_);
        deferred_.Resolve(result);
    }
    void OnError(const Napi::Error& e) override {
        busy_->store(false);
        deferred_.Reject(e.Value());
    }
    Napi::Promise GetPromise() { return deferred_.Promise(); }

private:
    std::shared_ptr<tnrp::Engine> engine_;
    std::string path_;
    std::shared_ptr<std::atomic<bool>> busy_;
    bool ok_ = false;
    std::string error_;
    Napi::Promise::Deferred deferred_;
};

// Indexed AL and finite-window prefixes are extracted on libuv's worker pool.
// Generation checks in Engine make rapid scrub requests monotonic even if
// workers begin out of order.
class PlayerSeekWorker : public Napi::AsyncWorker {
public:
    PlayerSeekWorker(Napi::Env env, std::shared_ptr<tnrp::Engine> engine,
                     float pct, bool allHistory, uint64_t requestId,
                     uint32_t rowTypeMask, float windowSeconds)
        : Napi::AsyncWorker(env), engine_(std::move(engine)), pct_(pct),
          allHistory_(allHistory), requestId_(requestId), rowTypeMask_(rowTypeMask),
          windowSeconds_(windowSeconds) {}
    void Execute() override { engine_->playerSeek(pct_, allHistory_, requestId_, rowTypeMask_, windowSeconds_); }
private:
    std::shared_ptr<tnrp::Engine> engine_;
    float pct_;
    bool allHistory_;
    uint64_t requestId_;
    uint32_t rowTypeMask_;
    float windowSeconds_;
};

class PlayerHistoryWorker : public Napi::AsyncWorker {
public:
    PlayerHistoryWorker(Napi::Env env, std::shared_ptr<tnrp::Engine> engine,
                        uint64_t requestId, uint32_t rowTypeMask,
                        float windowSeconds = 0.0f, bool windowRequest = false)
        : Napi::AsyncWorker(env), engine_(std::move(engine)), requestId_(requestId),
          rowTypeMask_(rowTypeMask), windowSeconds_(windowSeconds),
          windowRequest_(windowRequest) {}
    void Execute() override {
        if (windowRequest_) engine_->playerGetWindowData(windowSeconds_, requestId_, rowTypeMask_);
        else engine_->playerGetAllLapsData(requestId_, rowTypeMask_);
    }
private:
    std::shared_ptr<tnrp::Engine> engine_;
    uint64_t requestId_;
    uint32_t rowTypeMask_;
    float windowSeconds_;
    bool windowRequest_;
};

class DataRequirementsWorker : public Napi::AsyncWorker {
public:
    DataRequirementsWorker(Napi::Env env, std::shared_ptr<tnrp::Engine> engine,
                           uint32_t streamMask, uint32_t historyMask,
                           float windowSeconds, uint64_t requestId)
        : Napi::AsyncWorker(env), engine_(std::move(engine)),
          streamMask_(streamMask), historyMask_(historyMask),
          windowSeconds_(windowSeconds), requestId_(requestId) {}
    void Execute() override {
        engine_->setDataRequirements(streamMask_, historyMask_, windowSeconds_,
                                     requestId_);
    }
private:
    std::shared_ptr<tnrp::Engine> engine_;
    uint32_t streamMask_;
    uint32_t historyMask_;
    float windowSeconds_;
    uint64_t requestId_;
};

struct AnalysisReaderState {
    std::mutex mutex;
    tnrp::TnrdReader reader;
    std::atomic<bool> busy{false};
};

// Loads a second recording for Analysis without replacing or controlling the
// active playback reader.
class AnalysisLoadWorker : public Napi::AsyncWorker {
public:
    AnalysisLoadWorker(Napi::Env env, std::shared_ptr<AnalysisReaderState> state,
                       std::string path)
        : Napi::AsyncWorker(env), state_(std::move(state)), path_(std::move(path)),
          deferred_(Napi::Promise::Deferred::New(env)) {}

    void Execute() override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        tnrp::HeaderRow header;
        state_->reader.setLapStatusSummaries(true);
        ok_ = state_->reader.load(path_, header);
        if (ok_) {
            blocksJson_ = state_->reader.lapBlocksMessage();
            trackId_ = header.track_id;
            trackName_ = header.track_name;
        }
        else error_ = state_->reader.lastError();
    }

    void OnOK() override {
        state_->busy.store(false);
        Napi::Object result = Napi::Object::New(Env());
        result.Set("ok", ok_);
        if (ok_) {
            result.Set("blocksJson", blocksJson_);
            result.Set("trackId", trackId_);
            result.Set("trackName", trackName_);
        }
        else result.Set("error", error_);
        deferred_.Resolve(result);
    }

    void OnError(const Napi::Error& e) override {
        state_->busy.store(false);
        deferred_.Reject(e.Value());
    }

    Napi::Promise GetPromise() { return deferred_.Promise(); }

private:
    std::shared_ptr<AnalysisReaderState> state_;
    std::string path_, blocksJson_, trackName_, error_;
    bool ok_ = false;
    int trackId_ = 0;
    Napi::Promise::Deferred deferred_;
};

// Computes a complete lap-to-lap delta off the Electron main thread. The two
// laps may come from the active player or the independent secondary Analysis
// reader; the renderer sends identities only and never performs interpolation.
class AnalysisDeltaWorker : public Napi::AsyncWorker {
public:
    AnalysisDeltaWorker(Napi::Env env, std::shared_ptr<tnrp::Engine> engine,
                        std::shared_ptr<AnalysisReaderState> secondary,
                        int currentLap, bool currentSecondary,
                        int comparisonLap, bool comparisonSecondary,
                        bool sectorDelta)
        : Napi::AsyncWorker(env), engine_(std::move(engine)),
          secondary_(std::move(secondary)), currentLap_(currentLap),
          currentSecondary_(currentSecondary), comparisonLap_(comparisonLap),
          comparisonSecondary_(comparisonSecondary), sectorDelta_(sectorDelta),
          deferred_(Napi::Promise::Deferred::New(env)) {}

    void Execute() override {
        tnrp::AnalysisLapProgress current;
        tnrp::AnalysisLapProgress comparison;
        bool haveCurrent = false;
        bool haveComparison = false;

        if (currentSecondary_ && comparisonSecondary_) {
            std::lock_guard<std::mutex> lock(secondary_->mutex);
            haveCurrent = secondary_->reader.getAnalysisLapProgress(currentLap_, current);
            haveComparison = secondary_->reader.getAnalysisLapProgress(comparisonLap_, comparison);
        } else {
            if (currentSecondary_) {
                std::lock_guard<std::mutex> lock(secondary_->mutex);
                haveCurrent = secondary_->reader.getAnalysisLapProgress(currentLap_, current);
            } else if (engine_) {
                haveCurrent = engine_->playerGetAnalysisLapProgress(currentLap_, current);
            }
            if (comparisonSecondary_) {
                std::lock_guard<std::mutex> lock(secondary_->mutex);
                haveComparison = secondary_->reader.getAnalysisLapProgress(comparisonLap_, comparison);
            } else if (engine_) {
                haveComparison = engine_->playerGetAnalysisLapProgress(comparisonLap_, comparison);
            }
        }

        if (!haveCurrent || !haveComparison) return;
        json_ = tnrp::writeJson(tnrp::calculateLapDelta(current, comparison, sectorDelta_));
    }

    void OnOK() override { deferred_.Resolve(Napi::String::New(Env(), json_)); }
    void OnError(const Napi::Error& error) override { deferred_.Reject(error.Value()); }
    Napi::Promise GetPromise() { return deferred_.Promise(); }

private:
    std::shared_ptr<tnrp::Engine> engine_;
    std::shared_ptr<AnalysisReaderState> secondary_;
    int currentLap_;
    bool currentSecondary_;
    int comparisonLap_;
    bool comparisonSecondary_;
    bool sectorDelta_;
    std::string json_;
    Napi::Promise::Deferred deferred_;
};

class TNRPAddon : public Napi::ObjectWrap<TNRPAddon>, public tnrp::Sink {
public:
    static Napi::Object Init(Napi::Env env, Napi::Object exports) {
        TRACE("Init: before DefineClass");
        Napi::Function func = DefineClass(env, "Engine", {
            InstanceMethod("startUdp", &TNRPAddon::StartUdp),
            InstanceMethod("udpLastError", &TNRPAddon::UdpLastError),
            InstanceMethod("setDiagnosticsEnabled", &TNRPAddon::SetDiagnosticsEnabled),
            InstanceMethod("liveDiagnostics", &TNRPAddon::LiveDiagnostics),
            InstanceMethod("setOverride", &TNRPAddon::SetOverride),
            InstanceMethod("setTeamColorOverrides", &TNRPAddon::SetTeamColorOverrides),
            InstanceMethod("teamColorCatalog", &TNRPAddon::TeamColorCatalog),
            InstanceMethod("setStrategyMinimumStops", &TNRPAddon::SetStrategyMinimumStops),
            InstanceMethod("setLogging", &TNRPAddon::SetLogging),
            InstanceMethod("setLoggingZstd", &TNRPAddon::SetLoggingZstd),
            InstanceMethod("setLoggingGzip", &TNRPAddon::SetLoggingGzip),
            InstanceMethod("flushRecording", &TNRPAddon::FlushRecording),
            InstanceMethod("setDataRequirements", &TNRPAddon::SetDataRequirements),
            InstanceMethod("playerLoad", &TNRPAddon::PlayerLoad),
            InstanceMethod("playerPlay", &TNRPAddon::PlayerPlay),
            InstanceMethod("playerPause", &TNRPAddon::PlayerPause),
            InstanceMethod("playerSeek", &TNRPAddon::PlayerSeek),
            InstanceMethod("playerSetSpeed", &TNRPAddon::PlayerSetSpeed),
            InstanceMethod("playerGetLapData", &TNRPAddon::PlayerGetLapData),
            InstanceMethod("liveGetFastestLap", &TNRPAddon::LiveGetFastestLap),
            InstanceMethod("playerGetAllLapsData", &TNRPAddon::PlayerGetAllLapsData),
            InstanceMethod("playerGetWindowData", &TNRPAddon::PlayerGetWindowData),
            InstanceMethod("playerClose", &TNRPAddon::PlayerClose),
            InstanceMethod("analysisLoadFile", &TNRPAddon::AnalysisLoadFile),
            InstanceMethod("analysisGetLapData", &TNRPAddon::AnalysisGetLapData),
            InstanceMethod("analysisCompareLaps", &TNRPAddon::AnalysisCompareLaps),
            InstanceMethod("analysisCloseFile", &TNRPAddon::AnalysisCloseFile),
            InstanceMethod("playerExportXlsx", &TNRPAddon::PlayerExportXlsx),
            InstanceMethod("pairStart", &TNRPAddon::PairStart),
            InstanceMethod("pairStop", &TNRPAddon::PairStop),
            InstanceMethod("pairOpenWindow", &TNRPAddon::PairOpenWindow),
            InstanceMethod("pairCloseWindow", &TNRPAddon::PairCloseWindow),
            InstanceMethod("pairRemoveDevice", &TNRPAddon::PairRemoveDevice),
            InstanceMethod("pairGetState", &TNRPAddon::PairGetState),
            InstanceMethod("telemetryRetention", &TNRPAddon::TelemetryRetention),
            InstanceMethod("destroy", &TNRPAddon::Destroy)
        });
        TRACE("Init: after DefineClass");

        Napi::FunctionReference* constructor = new Napi::FunctionReference();
        *constructor = Napi::Persistent(func);
        TRACE("Init: after Persistent");
        env.SetInstanceData(constructor);
        TRACE("Init: after SetInstanceData");

        exports.Set("Engine", func);
        TRACE("Init: after exports.Set");
        return exports;
    }

    TNRPAddon(const Napi::CallbackInfo& info) : Napi::ObjectWrap<TNRPAddon>(info) {
        TRACE("TNRPAddon ctor: start");
        Napi::Env env = info.Env();

        // Expected args: (configObj, callback)
        if (info.Length() < 2 || !info[0].IsObject() || !info[1].IsFunction()) {
            Napi::TypeError::New(env, "Expected (configObj, callback)").ThrowAsJavaScriptException();
            return;
        }

        Napi::Object configObj = info[0].As<Napi::Object>();
        tnrp::Config config;

        if (configObj.Has("format") && configObj.Get("format").IsString()) {
            std::string fmt = configObj.Get("format").As<Napi::String>().Utf8Value();
            config.protocol = tnrp::overrideFromString(fmt);
        }
        if (configObj.Has("port") && configObj.Get("port").IsNumber()) {
            const double port = configObj.Get("port").As<Napi::Number>().DoubleValue();
            if (!std::isfinite(port) || std::trunc(port) != port || port < 1 || port > 65535) {
                Napi::RangeError::New(env, "UDP port must be an integer from 1 to 65535")
                    .ThrowAsJavaScriptException();
                return;
            }
            config.port = static_cast<uint16_t>(port);
        }
        if (configObj.Has("bindAddress") && configObj.Get("bindAddress").IsString()) {
            config.bindAddress = configObj.Get("bindAddress").As<Napi::String>().Utf8Value();
        }
        if (configObj.Has("forwardTargets") && configObj.Get("forwardTargets").IsArray()) {
            Napi::Array targets = configObj.Get("forwardTargets").As<Napi::Array>();
            const uint32_t count = std::min<uint32_t>(targets.Length(), tnrp::kMaxUdpForwardTargets);
            config.udpForwardTargets.reserve(count);
            for (uint32_t i = 0; i < count; ++i) {
                Napi::Value value = targets.Get(i);
                if (!value.IsObject()) continue;
                Napi::Object targetObj = value.As<Napi::Object>();
                if (!targetObj.Has("address") || !targetObj.Get("address").IsString() ||
                    !targetObj.Has("port") || !targetObj.Get("port").IsNumber()) continue;
                const uint32_t targetPort = targetObj.Get("port").As<Napi::Number>().Uint32Value();
                if (targetPort == 0 || targetPort > 65535) continue;
                config.udpForwardTargets.push_back({
                    targetObj.Get("address").As<Napi::String>().Utf8Value(),
                    static_cast<uint16_t>(targetPort)
                });
            }
        }
        if (configObj.Has("binaryPlayback") && configObj.Get("binaryPlayback").IsBoolean()) {
            config.binaryPlayback = configObj.Get("binaryPlayback").As<Napi::Boolean>().Value();
        }
        if (configObj.Has("strategyMinimumStops") && configObj.Get("strategyMinimumStops").IsNumber()) {
            config.strategyMinimumStops = configObj.Get("strategyMinimumStops").As<Napi::Number>().Int32Value();
        }
        if (configObj.Has("teamColorOverrides")) {
            config.teamColorOverrides = readTeamColorOverrides(
                configObj.Get("teamColorOverrides"));
        }
        if (configObj.Has("pairEnabled") && configObj.Get("pairEnabled").IsBoolean()) {
            config.pairEnabled = configObj.Get("pairEnabled").As<Napi::Boolean>().Value();
        }
        if (configObj.Has("pairPort") && configObj.Get("pairPort").IsNumber()) {
            const uint32_t port = configObj.Get("pairPort").As<Napi::Number>().Uint32Value();
            if (port > 0 && port <= 65535) config.pairPort = static_cast<uint16_t>(port);
        }
        if (configObj.Has("pairName") && configObj.Get("pairName").IsString()) {
            config.pairName = configObj.Get("pairName").As<Napi::String>().Utf8Value();
        }
        if (configObj.Has("pairStateJson") && configObj.Get("pairStateJson").IsString()) {
            config.pairStateJson = configObj.Get("pairStateJson").As<Napi::String>().Utf8Value();
        }
        TRACE("TNRPAddon ctor: config parsed");

        Napi::Function cb = info[1].As<Napi::Function>();

        // Create ThreadSafeFunction for the JSON (cold + control) row batch.
        tsfn = Napi::ThreadSafeFunction::New(
            env,
            cb,
            "TNRP Callback",
            0,
            1,
            [this](Napi::Env) {
                // Finalizer
            }
        );
        tsfn.Unref(env); // Allow the Node event loop to exit even if tsfn is active
        TRACE("TNRPAddon ctor: tsfn created");

        // Optional second callback for the hot-row binary batch (Buffer).
        if (info.Length() >= 3 && info[2].IsFunction()) {
            Napi::Function binCb = info[2].As<Napi::Function>();
            tsfnBin = Napi::ThreadSafeFunction::New(
                env, binCb, "TNRP Binary Callback", 0, 1, [](Napi::Env) {});
            tsfnBin.Unref(env);
            hasBinCb_ = true;
        }

        // Optional third callback for the playback seek flush
        // (binary: Buffer, coldJson: string, currentLapStart: number,
        //  lapNum: number, allHistory: boolean).
        // Only fires when the engine runs with config.binaryPlayback.
        if (info.Length() >= 4 && info[3].IsFunction()) {
            Napi::Function seekCb = info[3].As<Napi::Function>();
            tsfnSeek = Napi::ThreadSafeFunction::New(
                env, seekCb, "TNRP SeekFlush Callback", 0, 1, [](Napi::Env) {});
            tsfnSeek.Unref(env);
            hasSeekCb_ = true;
        }

        // Optional fourth callback for public paired-mode UI state plus the
        // opaque private state document that the host persists unchanged.
        if (info.Length() >= 5 && info[4].IsFunction()) {
            Napi::Function pairCb = info[4].As<Napi::Function>();
            tsfnPair = Napi::ThreadSafeFunction::New(
                env, pairCb, "TNRP Pair State Callback", 0, 1,
                [](Napi::Env) {});
            tsfnPair.Unref(env);
            hasPairCb_ = true;
        }

        // Optional fifth callback for low-volume native paired-transport
        // lifecycle diagnostics. It is intentionally independent of the UI
        // state callback so diagnostics never become persisted pair state.
        if (info.Length() >= 6 && info[5].IsFunction()) {
            Napi::Function pairDiagnosticCb = info[5].As<Napi::Function>();
            tsfnPairDiagnostic = Napi::ThreadSafeFunction::New(
                env, pairDiagnosticCb, "TNRP Pair Diagnostic Callback", 0, 1,
                [](Napi::Env) {});
            tsfnPairDiagnostic.Unref(env);
            hasPairDiagnosticCb_ = true;
        }
        TRACE("TNRPAddon ctor: about to construct Engine");

        engine = std::make_shared<tnrp::Engine>(config, this);
        TRACE("TNRPAddon ctor: Engine constructed, done");
    }

    ~TNRPAddon() {
        if (!destroyed_) {
            if (engine) {
                engine->pairStop(false);
                // ObjectWrap finalization can run while Node is dismantling the
                // N-API environment. playerClose() emits playback_close through
                // onRow(), so calling it here may touch an already-closing TSFN.
                // Normal application shutdown invokes Destroy() while N-API is
                // still live; this fallback must not emit playback rows.
                engine.reset();
            }
            tsfn.Release();
            if (hasBinCb_) tsfnBin.Release();
            if (hasSeekCb_) tsfnSeek.Release();
            if (hasPairCb_) tsfnPair.Release();
            if (hasPairDiagnosticCb_) tsfnPairDiagnostic.Release();
        }
    }

    // Rows arrive on the engine's UDP/playback thread at up to several hundred per
    // second. Rather than paying a cross-thread call + JS callback + heap copy per
    // row, we accumulate rows into a newline-delimited buffer and only schedule a
    // flush when the buffer transitions from empty. The JS-thread callback then
    // drains the whole buffer in one call. This naturally coalesces under load
    // (≤1 in-flight TSFN entry) while staying ~1:1 when the main thread keeps up.
    void onRow(const std::string& json) override {
        auto fs = flush_;  // keep state alive independent of this object's lifetime
        bool schedule = false;
        {
            std::lock_guard<std::mutex> lk(fs->mutex);
            fs->pending += json;
            fs->pending += '\n';
            ++fs->rowsEnqueued;
            fs->payloadBytesEnqueued += json.size() + 1;
            fs->peakPendingUsedBytes = std::max(fs->peakPendingUsedBytes, fs->pending.size());
            fs->peakPendingCapacityBytes = std::max(
                fs->peakPendingCapacityBytes, fs->pending.capacity());
            if (!fs->scheduled) {
                fs->scheduled = true;
                ++fs->scheduleAttempts;
                schedule = true;
            }
        }
        if (!schedule) return;

        auto status = tsfn.NonBlockingCall([fs](Napi::Env env, Napi::Function cb) {
            {
                std::lock_guard<std::mutex> lk(fs->mutex);
                fs->draining.swap(fs->pending);  // grab the batch; pending keeps reusable storage
                fs->scheduled = false;
                fs->peakDrainingUsedBytes = std::max(
                    fs->peakDrainingUsedBytes, fs->draining.size());
                fs->peakDrainingCapacityBytes = std::max(
                    fs->peakDrainingCapacityBytes, fs->draining.capacity());
            }
            if (env != nullptr && cb != nullptr) {
                cb.Call({ Napi::String::New(env, fs->draining) });
            }
            std::lock_guard<std::mutex> lk(fs->mutex);
            ++fs->deliveredBatches;
            fs->deliveredPayloadBytes += fs->draining.size();
            fs->draining.clear();                // retain capacity for the next swap
        });

        if (status != napi_ok) {
            // Couldn't schedule; clear the flag so a later row retries the flush.
            std::lock_guard<std::mutex> lk(fs->mutex);
            fs->scheduled = false;
        }
    }

    // Hot-row binary batch. Same coalescing strategy as onRow(), but accumulates
    // raw bytes and hands them to JS as a single Buffer per flush.
    void onBinary(const uint8_t* data, size_t len) override {
        if (!hasBinCb_ || len == 0) return;
        auto fs = binFlush_;
        bool schedule = false;
        uint64_t generation = 0;
        {
            std::lock_guard<std::mutex> lk(fs->mutex);
            if (fs->discard) return;
            fs->pending.insert(fs->pending.end(), data, data + len);
            ++fs->writesEnqueued;
            fs->payloadBytesEnqueued += len;
            fs->peakPendingUsedBytes = std::max(fs->peakPendingUsedBytes, fs->pending.size());
            fs->peakPendingCapacityBytes = std::max(
                fs->peakPendingCapacityBytes, fs->pending.capacity());
            generation = fs->generation;
            if (!fs->scheduled) {
                fs->scheduled = true;
                fs->scheduledGeneration = generation;
                ++fs->scheduleAttempts;
                schedule = true;
            }
        }
        if (!schedule) return;

        auto status = tsfnBin.NonBlockingCall([fs, generation](Napi::Env env, Napi::Function cb) {
            {
                std::lock_guard<std::mutex> lk(fs->mutex);
                // playerClose() invalidates a queued playback batch before the
                // JSON playback_close row is allowed to reach the renderer.
                // A later live batch may already own this shared flush state,
                // so the obsolete callback must leave it untouched.
                if (fs->generation != generation ||
                    fs->scheduledGeneration != generation) return;
                fs->draining.swap(fs->pending);  // grab the batch; pending keeps storage
                fs->scheduled = false;
                fs->peakDrainingUsedBytes = std::max(
                    fs->peakDrainingUsedBytes, fs->draining.size());
                fs->peakDrainingCapacityBytes = std::max(
                    fs->peakDrainingCapacityBytes, fs->draining.capacity());
            }
            if (env != nullptr && cb != nullptr) {
                cb.Call({ Napi::Buffer<uint8_t>::Copy(env, fs->draining.data(), fs->draining.size()) });
            }
            std::lock_guard<std::mutex> lk(fs->mutex);
            ++fs->deliveredBatches;
            fs->deliveredPayloadBytes += fs->draining.size();
            fs->draining.clear();                // retain capacity for the next swap
        });

        if (status != napi_ok) {
            std::lock_guard<std::mutex> lk(fs->mutex);
            if (fs->scheduledGeneration == generation) fs->scheduled = false;
        }
    }

    // Playback seek flush (Config::binaryPlayback only). The reader hands us a
    // zero-copy view of its immutable packed store. Copy once into a normal V8
    // Buffer here: Electron IPC cannot serialize an external-memory Buffer.
    void onSeekFlush(std::shared_ptr<const std::vector<uint8_t>> binStore,
                     size_t binBegin, size_t binEnd, std::string&& coldJson,
                     float currentLapStart, int lapNum, bool allHistory,
                     uint64_t requestId, bool authoritativeSeek,
                     uint32_t rowTypeMask, float historyStart) override {
        if (!hasSeekCb_) return;
        struct SeekData {
            std::shared_ptr<const std::vector<uint8_t>> binStore;
            size_t binBegin;
            size_t binEnd;
            std::string cold;
            std::shared_ptr<std::atomic<size_t>> retainedCounter;
            size_t retainedBytes;
            float lapStart;
            int lapNum;
            bool allHistory;
            uint64_t requestId;
            bool authoritativeSeek;
            uint32_t rowTypeMask;
            float historyStart;
        };
        const size_t binaryBytes = binStore && binEnd > binBegin ? binEnd - binBegin : 0;
        const size_t retainedBytes = binaryBytes + coldJson.capacity();
        seekFlushBytes_->fetch_add(retainedBytes, std::memory_order_relaxed);
        seekFlushCalls_->fetch_add(1, std::memory_order_relaxed);
        seekFlushPayloadBytes_->fetch_add(retainedBytes, std::memory_order_relaxed);
        updateAtomicMaximum(*seekFlushPeakBytes_, seekFlushBytes_->load(std::memory_order_relaxed));
        auto* d = new SeekData{ std::move(binStore), binBegin, binEnd, std::move(coldJson),
                                seekFlushBytes_, retainedBytes,
                                currentLapStart, lapNum, allHistory, requestId,
                                authoritativeSeek, rowTypeMask, historyStart };
        auto status = tsfnSeek.NonBlockingCall(
            d, [](Napi::Env env, Napi::Function cb, SeekData* d) {
                if (env != nullptr && cb != nullptr) {
                    const size_t len = d->binStore && d->binEnd > d->binBegin
                        ? d->binEnd - d->binBegin : 0;
                    auto buffer = len == 0
                        ? Napi::Buffer<uint8_t>::New(env, 0)
                        : Napi::Buffer<uint8_t>::Copy(
                            env, d->binStore->data() + d->binBegin, len);
                    cb.Call({ buffer,
                              Napi::String::New(env, d->cold),
                              Napi::Number::New(env, d->lapStart),
                              Napi::Number::New(env, d->lapNum),
                              Napi::Boolean::New(env, d->allHistory),
                              Napi::Number::New(env, static_cast<double>(d->requestId)),
                              Napi::Boolean::New(env, d->authoritativeSeek),
                              Napi::Number::New(env, d->rowTypeMask),
                              Napi::Number::New(env, d->historyStart) });
                }
                d->retainedCounter->fetch_sub(d->retainedBytes, std::memory_order_relaxed);
                delete d;
            });
        if (status != napi_ok) {
            d->retainedCounter->fetch_sub(d->retainedBytes, std::memory_order_relaxed);
            delete d;
        }
    }

    void onPairState(const std::string& publicJson,
                     const std::string& persistedJson) override {
        if (!hasPairCb_) return;
        struct PairStateData {
            std::string publicJson;
            std::string persistedJson;
        };
        auto* data = new PairStateData{publicJson, persistedJson};
        const auto status = tsfnPair.NonBlockingCall(
            data, [](Napi::Env env, Napi::Function callback,
                     PairStateData* state) {
                if (env != nullptr && callback != nullptr) {
                    callback.Call({Napi::String::New(env, state->publicJson),
                                   Napi::String::New(env, state->persistedJson)});
                }
                delete state;
            });
        if (status != napi_ok) delete data;
    }

    void onPairDiagnostic(const std::string& message) override {
        if (!hasPairDiagnosticCb_) return;
        auto* data = new std::string(message);
        const auto status = tsfnPairDiagnostic.NonBlockingCall(
            data, [](Napi::Env env, Napi::Function callback,
                     std::string* diagnostic) {
                if (env != nullptr && callback != nullptr)
                    callback.Call({Napi::String::New(env, *diagnostic)});
                delete diagnostic;
            });
        if (status != napi_ok) delete data;
    }

private:
    // Shared so queued flush callbacks remain valid even if the wrapper is torn down.
    struct FlushState {
        std::mutex  mutex;
        std::string pending;    // newline-delimited JSON awaiting delivery
        std::string draining;   // batch currently being handed to JS (reused storage)
        bool        scheduled = false;
        uint64_t    rowsEnqueued{};
        uint64_t    payloadBytesEnqueued{};
        uint64_t    scheduleAttempts{};
        uint64_t    deliveredBatches{};
        uint64_t    deliveredPayloadBytes{};
        size_t      peakPendingUsedBytes{};
        size_t      peakPendingCapacityBytes{};
        size_t      peakDrainingUsedBytes{};
        size_t      peakDrainingCapacityBytes{};
    };

    // Shared so queued binary flush callbacks remain valid past teardown.
    struct BinFlushState {
        std::mutex           mutex;
        std::vector<uint8_t> pending;    // bytes awaiting delivery
        std::vector<uint8_t> draining;   // batch currently handed to JS (reused storage)
        bool                 scheduled = false;
        bool                 discard = false;
        uint64_t             generation = 0;
        uint64_t             scheduledGeneration = 0;
        uint64_t             writesEnqueued{};
        uint64_t             payloadBytesEnqueued{};
        uint64_t             scheduleAttempts{};
        uint64_t             deliveredBatches{};
        uint64_t             deliveredPayloadBytes{};
        size_t               peakPendingUsedBytes{};
        size_t               peakPendingCapacityBytes{};
        size_t               peakDrainingUsedBytes{};
        size_t               peakDrainingCapacityBytes{};
    };

    std::shared_ptr<tnrp::Engine> engine;
    Napi::ThreadSafeFunction tsfn;
    Napi::ThreadSafeFunction tsfnBin;
    Napi::ThreadSafeFunction tsfnSeek;
    Napi::ThreadSafeFunction tsfnPair;
    Napi::ThreadSafeFunction tsfnPairDiagnostic;
    bool destroyed_ = false;
    bool hasBinCb_  = false;
    bool hasSeekCb_ = false;
    bool hasPairCb_ = false;
    bool hasPairDiagnosticCb_ = false;
    std::shared_ptr<FlushState>    flush_    = std::make_shared<FlushState>();
    std::shared_ptr<BinFlushState> binFlush_ = std::make_shared<BinFlushState>();
    std::shared_ptr<std::atomic<size_t>> seekFlushBytes_ =
        std::make_shared<std::atomic<size_t>>(0);
    std::shared_ptr<std::atomic<size_t>> seekFlushPeakBytes_ =
        std::make_shared<std::atomic<size_t>>(0);
    std::shared_ptr<std::atomic<uint64_t>> seekFlushCalls_ =
        std::make_shared<std::atomic<uint64_t>>(0);
    std::shared_ptr<std::atomic<uint64_t>> seekFlushPayloadBytes_ =
        std::make_shared<std::atomic<uint64_t>>(0);
    std::shared_ptr<std::atomic<bool>> loadBusy_ = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<AnalysisReaderState> analysisReader_ = std::make_shared<AnalysisReaderState>();

    Napi::Value StartUdp(const Napi::CallbackInfo& info) {
        TRACE("StartUdp: calling engine->startUdp()");
        bool ok = engine->startUdp();
        TRACE("StartUdp: returned");
        return Napi::Boolean::New(info.Env(), ok);
    }

    Napi::Value PairStart(const Napi::CallbackInfo& info) {
        std::string error;
        if (engine && engine->pairStart(&error)) return info.Env().Null();
        return Napi::String::New(info.Env(), error.empty()
            ? "The paired-display server is unavailable." : error);
    }

    Napi::Value PairStop(const Napi::CallbackInfo& info) {
        const bool persistDisabled = info.Length() == 0 || !info[0].IsBoolean() ||
            info[0].As<Napi::Boolean>().Value();
        if (engine) engine->pairStop(persistDisabled);
        return PairGetState(info);
    }

    Napi::Value PairOpenWindow(const Napi::CallbackInfo& info) {
        if (engine) engine->pairOpenWindow();
        return PairGetState(info);
    }

    Napi::Value PairCloseWindow(const Napi::CallbackInfo& info) {
        if (engine) engine->pairCloseWindow();
        return PairGetState(info);
    }

    Napi::Value PairRemoveDevice(const Napi::CallbackInfo& info) {
        if (engine && info.Length() >= 1 && info[0].IsString())
            engine->pairRemoveDevice(info[0].As<Napi::String>().Utf8Value());
        return PairGetState(info);
    }

    Napi::Value PairGetState(const Napi::CallbackInfo& info) {
        return Napi::String::New(info.Env(), engine
            ? engine->pairStateJson() : "{}");
    }

    Napi::Value UdpLastError(const Napi::CallbackInfo& info) {
        return Napi::String::New(info.Env(), engine->udpLastError());
    }

    void SetDiagnosticsEnabled(const Napi::CallbackInfo& info) {
        if (engine && info.Length() >= 1)
            engine->setDiagnosticsEnabled(info[0].ToBoolean().Value());
    }

    Napi::Value LiveDiagnostics(const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();
        Napi::Object out = Napi::Object::New(env);
        if (!engine) return out;
        const auto snapshot = engine->liveDiagnostics();
        out.Set("udpRunning", snapshot.udpRunning);
        out.Set("inPlayback", snapshot.inPlayback);
        out.Set("recording", snapshot.recording);
        out.Set("datagrams", Napi::Number::New(env, static_cast<double>(snapshot.datagrams)));
        out.Set("bytes", Napi::Number::New(env, static_cast<double>(snapshot.bytes)));
        out.Set("tooShort", Napi::Number::New(env, static_cast<double>(snapshot.tooShort)));
        out.Set("unsupportedFormat", Napi::Number::New(env, static_cast<double>(snapshot.unsupportedFormat)));
        out.Set("parserDropped", Napi::Number::New(env, static_cast<double>(snapshot.parserDropped)));
        out.Set("accepted", Napi::Number::New(env, static_cast<double>(snapshot.accepted)));
        out.Set("rowsProduced", Napi::Number::New(env, static_cast<double>(snapshot.rowsProduced)));
        out.Set("binaryBytesProduced", Napi::Number::New(env, static_cast<double>(snapshot.binaryBytesProduced)));
        out.Set("noOutput", Napi::Number::New(env, static_cast<double>(snapshot.noOutput)));
        Napi::Object packetIds = Napi::Object::New(env);
        for (size_t i = 0; i < snapshot.packetIds.size(); ++i) {
            if (snapshot.packetIds[i] == 0) continue;
            packetIds.Set(i == 17 ? "other" : std::to_string(i),
                Napi::Number::New(env, static_cast<double>(snapshot.packetIds[i])));
        }
        out.Set("packetIds", packetIds);
        Napi::Object formats = Napi::Object::New(env);
        formats.Set("2024", Napi::Number::New(env, static_cast<double>(snapshot.format2024)));
        formats.Set("2025", Napi::Number::New(env, static_cast<double>(snapshot.format2025)));
        formats.Set("2026", Napi::Number::New(env, static_cast<double>(snapshot.format2026)));
        out.Set("formats", formats);
        out.Set("lastIncomingFormat", snapshot.lastIncomingFormat);
        out.Set("lastPacketId", snapshot.lastPacketId);
        out.Set("lastDatagramLength", snapshot.lastDatagramLength);
        out.Set("lastSessionTime", snapshot.lastSessionTime);
        out.Set("consumerRowMask", snapshot.consumerRowMask);
        out.Set("consumerHistoryMask", snapshot.consumerHistoryMask);
        out.Set("consumerWindowSeconds", snapshot.consumerWindowSeconds);
        return out;
    }

    Napi::Value SetOverride(const Napi::CallbackInfo& info) {
        if (info.Length() >= 1 && info[0].IsString()) {
            std::string ovr = info[0].As<Napi::String>().Utf8Value();
            if (ovr == "auto") engine->setOverride(tnrp::Override::Auto);
            else if (ovr == "f1_24") engine->setOverride(tnrp::Override::F1_24);
            else if (ovr == "f1_25") engine->setOverride(tnrp::Override::F1_25);
            else if (ovr == "f1_26") engine->setOverride(tnrp::Override::F1_26);
        }
        return info.Env().Undefined();
    }

    Napi::Value TelemetryRetention(const Napi::CallbackInfo& info) {
        size_t jsonPendingBytes = 0;
        size_t jsonDrainingBytes = 0;
        size_t jsonPendingUsed = 0;
        size_t jsonDrainingUsed = 0;
        uint64_t jsonRowsEnqueued = 0;
        uint64_t jsonPayloadBytesEnqueued = 0;
        uint64_t jsonScheduleAttempts = 0;
        uint64_t jsonDeliveredBatches = 0;
        uint64_t jsonDeliveredPayloadBytes = 0;
        size_t jsonPeakPendingUsedBytes = 0;
        size_t jsonPeakPendingCapacityBytes = 0;
        size_t jsonPeakDrainingUsedBytes = 0;
        size_t jsonPeakDrainingCapacityBytes = 0;
        {
            std::lock_guard<std::mutex> lock(flush_->mutex);
            jsonPendingBytes = flush_->pending.capacity();
            jsonDrainingBytes = flush_->draining.capacity();
            jsonPendingUsed = flush_->pending.size();
            jsonDrainingUsed = flush_->draining.size();
            jsonRowsEnqueued = flush_->rowsEnqueued;
            jsonPayloadBytesEnqueued = flush_->payloadBytesEnqueued;
            jsonScheduleAttempts = flush_->scheduleAttempts;
            jsonDeliveredBatches = flush_->deliveredBatches;
            jsonDeliveredPayloadBytes = flush_->deliveredPayloadBytes;
            jsonPeakPendingUsedBytes = flush_->peakPendingUsedBytes;
            jsonPeakPendingCapacityBytes = flush_->peakPendingCapacityBytes;
            jsonPeakDrainingUsedBytes = flush_->peakDrainingUsedBytes;
            jsonPeakDrainingCapacityBytes = flush_->peakDrainingCapacityBytes;
        }

        size_t binaryPendingBytes = 0;
        size_t binaryDrainingBytes = 0;
        size_t binaryPendingUsed = 0;
        size_t binaryDrainingUsed = 0;
        uint64_t binaryWritesEnqueued = 0;
        uint64_t binaryPayloadBytesEnqueued = 0;
        uint64_t binaryScheduleAttempts = 0;
        uint64_t binaryDeliveredBatches = 0;
        uint64_t binaryDeliveredPayloadBytes = 0;
        size_t binaryPeakPendingUsedBytes = 0;
        size_t binaryPeakPendingCapacityBytes = 0;
        size_t binaryPeakDrainingUsedBytes = 0;
        size_t binaryPeakDrainingCapacityBytes = 0;
        {
            std::lock_guard<std::mutex> lock(binFlush_->mutex);
            binaryPendingBytes = binFlush_->pending.capacity();
            binaryDrainingBytes = binFlush_->draining.capacity();
            binaryPendingUsed = binFlush_->pending.size();
            binaryDrainingUsed = binFlush_->draining.size();
            binaryWritesEnqueued = binFlush_->writesEnqueued;
            binaryPayloadBytesEnqueued = binFlush_->payloadBytesEnqueued;
            binaryScheduleAttempts = binFlush_->scheduleAttempts;
            binaryDeliveredBatches = binFlush_->deliveredBatches;
            binaryDeliveredPayloadBytes = binFlush_->deliveredPayloadBytes;
            binaryPeakPendingUsedBytes = binFlush_->peakPendingUsedBytes;
            binaryPeakPendingCapacityBytes = binFlush_->peakPendingCapacityBytes;
            binaryPeakDrainingUsedBytes = binFlush_->peakDrainingUsedBytes;
            binaryPeakDrainingCapacityBytes = binFlush_->peakDrainingCapacityBytes;
        }

        const size_t seekBytes = seekFlushBytes_->load(std::memory_order_relaxed);
        const auto history = engine
            ? engine->liveHistoryMemoryStats()
            : tnrp::Engine::LiveHistoryMemoryStats{};
        const auto writer = engine
            ? engine->writerMemoryStats()
            : tnrp::TnrdWriter::MemoryStats{};
        const auto strategy = engine
            ? engine->strategyMemoryStats()
            : tnrp::Engine::StrategyMemoryStats{};
        const auto runtime = engine
            ? engine->runtimeMemoryStats()
            : tnrp::Engine::RuntimeMemoryStats{};
        const size_t retainedBytes = jsonPendingBytes + jsonDrainingBytes +
            binaryPendingBytes + binaryDrainingBytes + seekBytes + writer.retainedBytes +
            strategy.retainedBytes + runtime.retainedBytes;
        Napi::Object result = Napi::Object::New(info.Env());
        result.Set("retained_bytes", Napi::Number::New(info.Env(),
            static_cast<double>(retainedBytes)));
        result.Set("byte_basis", Napi::String::New(info.Env(),
            "reserved native flush/seek payload plus estimated engine-cache, writer, and Strategy retained allocation capacity; allocator overhead and transient allocations are reported separately, not added to retained_bytes"));

        Napi::Object json = Napi::Object::New(info.Env());
        json.Set("pending_bytes", Napi::Number::New(info.Env(),
            static_cast<double>(jsonPendingBytes)));
        json.Set("pending_used_bytes", Napi::Number::New(info.Env(),
            static_cast<double>(jsonPendingUsed)));
        json.Set("draining_bytes", Napi::Number::New(info.Env(),
            static_cast<double>(jsonDrainingBytes)));
        json.Set("draining_used_bytes", Napi::Number::New(info.Env(),
            static_cast<double>(jsonDrainingUsed)));
        Napi::Object jsonActivity = Napi::Object::New(info.Env());
        jsonActivity.Set("rows_enqueued", Napi::Number::New(info.Env(), static_cast<double>(jsonRowsEnqueued)));
        jsonActivity.Set("payload_bytes_enqueued", Napi::Number::New(info.Env(), static_cast<double>(jsonPayloadBytesEnqueued)));
        jsonActivity.Set("schedule_attempts", Napi::Number::New(info.Env(), static_cast<double>(jsonScheduleAttempts)));
        jsonActivity.Set("delivered_batches", Napi::Number::New(info.Env(), static_cast<double>(jsonDeliveredBatches)));
        jsonActivity.Set("delivered_payload_bytes", Napi::Number::New(info.Env(), static_cast<double>(jsonDeliveredPayloadBytes)));
        jsonActivity.Set("peak_pending_used_bytes", Napi::Number::New(info.Env(), static_cast<double>(jsonPeakPendingUsedBytes)));
        jsonActivity.Set("peak_pending_capacity_bytes", Napi::Number::New(info.Env(), static_cast<double>(jsonPeakPendingCapacityBytes)));
        jsonActivity.Set("peak_draining_used_bytes", Napi::Number::New(info.Env(), static_cast<double>(jsonPeakDrainingUsedBytes)));
        jsonActivity.Set("peak_draining_capacity_bytes", Napi::Number::New(info.Env(), static_cast<double>(jsonPeakDrainingCapacityBytes)));
        json.Set("activity", jsonActivity);
        result.Set("json_flush", json);

        Napi::Object binary = Napi::Object::New(info.Env());
        binary.Set("pending_bytes", Napi::Number::New(info.Env(),
            static_cast<double>(binaryPendingBytes)));
        binary.Set("pending_used_bytes", Napi::Number::New(info.Env(),
            static_cast<double>(binaryPendingUsed)));
        binary.Set("draining_bytes", Napi::Number::New(info.Env(),
            static_cast<double>(binaryDrainingBytes)));
        binary.Set("draining_used_bytes", Napi::Number::New(info.Env(),
            static_cast<double>(binaryDrainingUsed)));
        Napi::Object binaryActivity = Napi::Object::New(info.Env());
        binaryActivity.Set("writes_enqueued", Napi::Number::New(info.Env(), static_cast<double>(binaryWritesEnqueued)));
        binaryActivity.Set("payload_bytes_enqueued", Napi::Number::New(info.Env(), static_cast<double>(binaryPayloadBytesEnqueued)));
        binaryActivity.Set("schedule_attempts", Napi::Number::New(info.Env(), static_cast<double>(binaryScheduleAttempts)));
        binaryActivity.Set("delivered_batches", Napi::Number::New(info.Env(), static_cast<double>(binaryDeliveredBatches)));
        binaryActivity.Set("delivered_payload_bytes", Napi::Number::New(info.Env(), static_cast<double>(binaryDeliveredPayloadBytes)));
        binaryActivity.Set("peak_pending_used_bytes", Napi::Number::New(info.Env(), static_cast<double>(binaryPeakPendingUsedBytes)));
        binaryActivity.Set("peak_pending_capacity_bytes", Napi::Number::New(info.Env(), static_cast<double>(binaryPeakPendingCapacityBytes)));
        binaryActivity.Set("peak_draining_used_bytes", Napi::Number::New(info.Env(), static_cast<double>(binaryPeakDrainingUsedBytes)));
        binaryActivity.Set("peak_draining_capacity_bytes", Napi::Number::New(info.Env(), static_cast<double>(binaryPeakDrainingCapacityBytes)));
        binary.Set("activity", binaryActivity);
        result.Set("binary_flush", binary);
        result.Set("seek_flush_in_flight_bytes", Napi::Number::New(info.Env(),
            static_cast<double>(seekBytes)));
        Napi::Object seekActivity = Napi::Object::New(info.Env());
        seekActivity.Set("calls", Napi::Number::New(info.Env(), static_cast<double>(seekFlushCalls_->load(std::memory_order_relaxed))));
        seekActivity.Set("payload_bytes_created", Napi::Number::New(info.Env(), static_cast<double>(seekFlushPayloadBytes_->load(std::memory_order_relaxed))));
        seekActivity.Set("peak_in_flight_bytes", Napi::Number::New(info.Env(), static_cast<double>(seekFlushPeakBytes_->load(std::memory_order_relaxed))));
        result.Set("seek_flush_activity", seekActivity);

        Napi::Object runtimeStats = Napi::Object::New(info.Env());
        runtimeStats.Set("retained_bytes", Napi::Number::New(info.Env(), static_cast<double>(runtime.retainedBytes)));
        runtimeStats.Set("estimate_basis", Napi::String::New(info.Env(),
            "duplicate/latest-row/playback-path string capacities; transient parser-result and filtered-binary allocation activity is reported separately"));
        Napi::Object engineCaches = Napi::Object::New(info.Env());
        engineCaches.Set("duplicate_used_bytes", Napi::Number::New(info.Env(), static_cast<double>(runtime.duplicateCacheUsedBytes)));
        engineCaches.Set("duplicate_capacity_bytes", Napi::Number::New(info.Env(), static_cast<double>(runtime.duplicateCacheCapacityBytes)));
        engineCaches.Set("latest_row_used_bytes", Napi::Number::New(info.Env(), static_cast<double>(runtime.latestRowCacheUsedBytes)));
        engineCaches.Set("latest_row_capacity_bytes", Napi::Number::New(info.Env(), static_cast<double>(runtime.latestRowCacheCapacityBytes)));
        engineCaches.Set("playback_path_capacity_bytes", Napi::Number::New(info.Env(), static_cast<double>(runtime.playbackPathCapacityBytes)));
        runtimeStats.Set("caches", engineCaches);
        Napi::Object parserActivity = Napi::Object::New(info.Env());
        parserActivity.Set("datagrams_processed", Napi::Number::New(info.Env(), static_cast<double>(runtime.datagramsProcessed)));
        parserActivity.Set("datagram_bytes_processed", Napi::Number::New(info.Env(), static_cast<double>(runtime.datagramBytesProcessed)));
        parserActivity.Set("rows_produced", Napi::Number::New(info.Env(), static_cast<double>(runtime.parserRowsProduced)));
        parserActivity.Set("control_rows_produced", Napi::Number::New(info.Env(), static_cast<double>(runtime.parserControlRowsProduced)));
        parserActivity.Set("hot_json_rows_produced", Napi::Number::New(info.Env(), static_cast<double>(runtime.parserHotJsonRowsProduced)));
        parserActivity.Set("json_bytes_produced", Napi::Number::New(info.Env(), static_cast<double>(runtime.parserJsonBytesProduced)));
        parserActivity.Set("binary_bytes_produced", Napi::Number::New(info.Env(), static_cast<double>(runtime.parserBinaryBytesProduced)));
        parserActivity.Set("result_capacity_bytes_allocated", Napi::Number::New(info.Env(), static_cast<double>(runtime.parserResultCapacityBytesAllocated)));
        parserActivity.Set("last_result_capacity_bytes", Napi::Number::New(info.Env(), static_cast<double>(runtime.lastParserResultCapacityBytes)));
        parserActivity.Set("peak_result_capacity_bytes", Napi::Number::New(info.Env(), static_cast<double>(runtime.peakParserResultCapacityBytes)));
        runtimeStats.Set("parser_activity", parserActivity);
        Napi::Object filteredBinaryActivity = Napi::Object::New(info.Env());
        filteredBinaryActivity.Set("batches", Napi::Number::New(info.Env(), static_cast<double>(runtime.filteredBinaryBatches)));
        filteredBinaryActivity.Set("bytes_produced", Napi::Number::New(info.Env(), static_cast<double>(runtime.filteredBinaryBytesProduced)));
        filteredBinaryActivity.Set("capacity_bytes_allocated", Napi::Number::New(info.Env(), static_cast<double>(runtime.filteredBinaryCapacityBytesAllocated)));
        filteredBinaryActivity.Set("last_capacity_bytes", Napi::Number::New(info.Env(), static_cast<double>(runtime.lastFilteredBinaryCapacityBytes)));
        filteredBinaryActivity.Set("peak_capacity_bytes", Napi::Number::New(info.Env(), static_cast<double>(runtime.peakFilteredBinaryCapacityBytes)));
        runtimeStats.Set("filtered_binary_activity", filteredBinaryActivity);
        result.Set("engine_runtime", runtimeStats);

        Napi::Object strategyStats = Napi::Object::New(info.Env());
        strategyStats.Set("subscribed", Napi::Boolean::New(info.Env(), strategy.subscribed));
        strategyStats.Set("retained_bytes", Napi::Number::New(info.Env(), static_cast<double>(strategy.retainedBytes)));
        strategyStats.Set("estimate_basis", Napi::String::New(info.Env(),
            "Strategy processor, rollback checkpoints/journal, queued/active work payload capacities, and cached snapshots; shared JSON may overlap live-history and work attribution"));
        strategyStats.Set("cache_capacity_bytes", Napi::Number::New(info.Env(), static_cast<double>(strategy.cacheCapacityBytes)));
        Napi::Object strategyQueue = Napi::Object::New(info.Env());
        strategyQueue.Set("work_items", Napi::Number::New(info.Env(), static_cast<double>(strategy.queuedWorkItems)));
        strategyQueue.Set("rows", Napi::Number::New(info.Env(), static_cast<double>(strategy.queuedRows)));
        strategyQueue.Set("json_bytes", Napi::Number::New(info.Env(), static_cast<double>(strategy.queuedJsonBytes)));
        strategyQueue.Set("retained_bytes", Napi::Number::New(info.Env(), static_cast<double>(strategy.queuedRetainedBytes)));
        strategyQueue.Set("oldest_work_age_ms", Napi::Number::New(info.Env(), static_cast<double>(strategy.oldestQueuedWorkAgeMs)));
        strategyQueue.Set("peak_rows", Napi::Number::New(info.Env(), static_cast<double>(strategy.peakQueuedRows)));
        strategyQueue.Set("peak_retained_bytes", Napi::Number::New(info.Env(), static_cast<double>(strategy.peakQueuedRetainedBytes)));
        strategyStats.Set("work_queue", strategyQueue);
        Napi::Object strategyActive = Napi::Object::New(info.Env());
        strategyActive.Set("work_items", Napi::Number::New(info.Env(), static_cast<double>(strategy.activeWorkItems)));
        strategyActive.Set("rows", Napi::Number::New(info.Env(), static_cast<double>(strategy.activeRows)));
        strategyActive.Set("json_bytes", Napi::Number::New(info.Env(), static_cast<double>(strategy.activeJsonBytes)));
        strategyActive.Set("retained_bytes", Napi::Number::New(info.Env(), static_cast<double>(strategy.activeRetainedBytes)));
        strategyStats.Set("active_work", strategyActive);
        Napi::Object strategyRollback = Napi::Object::New(info.Env());
        strategyRollback.Set("retained_bytes", Napi::Number::New(info.Env(), static_cast<double>(strategy.rollback.retainedBytes)));
        strategyRollback.Set("checkpoints", Napi::Number::New(info.Env(), static_cast<double>(strategy.rollback.checkpoints)));
        strategyRollback.Set("journal_entries", Napi::Number::New(info.Env(), static_cast<double>(strategy.rollback.rows)));
        strategyRollback.Set("rollbacks", Napi::Number::New(info.Env(), static_cast<double>(strategy.rollback.rollbacks)));
        strategyRollback.Set("replayed_rows", Napi::Number::New(info.Env(), static_cast<double>(strategy.rollback.replayedRows)));
        strategyRollback.Set("fallbacks", Napi::Number::New(info.Env(), static_cast<double>(strategy.rollback.fallbacks)));
        strategyStats.Set("rollback", strategyRollback);
        Napi::Object strategyActivity = Napi::Object::New(info.Env());
        strategyActivity.Set("input_rows_enqueued", Napi::Number::New(info.Env(), static_cast<double>(strategy.inputRowsEnqueued)));
        strategyActivity.Set("input_json_bytes_enqueued", Napi::Number::New(info.Env(), static_cast<double>(strategy.inputJsonBytesEnqueued)));
        strategyActivity.Set("rows_processed", Napi::Number::New(info.Env(), static_cast<double>(strategy.rowsProcessed)));
        strategyActivity.Set("json_bytes_processed", Napi::Number::New(info.Env(), static_cast<double>(strategy.jsonBytesProcessed)));
        strategyActivity.Set("snapshots_generated", Napi::Number::New(info.Env(), static_cast<double>(strategy.snapshotsGenerated)));
        strategyActivity.Set("snapshots_emitted", Napi::Number::New(info.Env(), static_cast<double>(strategy.snapshotsEmitted)));
        strategyActivity.Set("snapshot_json_bytes_generated", Napi::Number::New(info.Env(), static_cast<double>(strategy.snapshotJsonBytesGenerated)));
        strategyActivity.Set("last_snapshot_json_bytes", Napi::Number::New(info.Env(), static_cast<double>(strategy.lastSnapshotJsonBytes)));
        strategyActivity.Set("last_snapshot_json_capacity_bytes", Napi::Number::New(info.Env(), static_cast<double>(strategy.lastSnapshotJsonCapacityBytes)));
        strategyActivity.Set("peak_snapshot_json_bytes", Napi::Number::New(info.Env(), static_cast<double>(strategy.peakSnapshotJsonBytes)));
        strategyStats.Set("activity", strategyActivity);
        Napi::Object strategyProcessor = Napi::Object::New(info.Env());
        strategyProcessor.Set("retained_bytes", Napi::Number::New(info.Env(), static_cast<double>(strategy.processor.retainedBytes)));
        strategyProcessor.Set("cached_input_capacity_bytes", Napi::Number::New(info.Env(), static_cast<double>(strategy.processor.cachedInputCapacityBytes)));
        strategyProcessor.Set("container_capacity_bytes", Napi::Number::New(info.Env(), static_cast<double>(strategy.processor.containerCapacityBytes)));
        strategyProcessor.Set("string_capacity_bytes", Napi::Number::New(info.Env(), static_cast<double>(strategy.processor.stringCapacityBytes)));
        strategyProcessor.Set("lap_time_entries", Napi::Number::New(info.Env(), static_cast<double>(strategy.processor.lapTimeEntries)));
        strategyProcessor.Set("rival_entries", Napi::Number::New(info.Env(), static_cast<double>(strategy.processor.rivalEntries)));
        strategyProcessor.Set("rival_recent_lap_entries", Napi::Number::New(info.Env(), static_cast<double>(strategy.processor.rivalRecentLapEntries)));
        strategyProcessor.Set("wear_history_entries", Napi::Number::New(info.Env(), static_cast<double>(strategy.processor.wearHistoryEntries)));
        strategyProcessor.Set("frozen_neutral_car_entries", Napi::Number::New(info.Env(), static_cast<double>(strategy.processor.frozenNeutralCarEntries)));
        strategyProcessor.Set("decision_history_entries", Napi::Number::New(info.Env(), static_cast<double>(strategy.processor.decisionHistoryEntries)));
        strategyProcessor.Set("conservative_past_entries", Napi::Number::New(info.Env(), static_cast<double>(strategy.processor.conservativePastEntries)));
        strategyProcessor.Set("aggressive_past_entries", Napi::Number::New(info.Env(), static_cast<double>(strategy.processor.aggressivePastEntries)));
        strategyProcessor.Set("required_lap_entries", Napi::Number::New(info.Env(), static_cast<double>(strategy.processor.requiredLapEntries)));
        strategyProcessor.Set("display_lap_entries", Napi::Number::New(info.Env(), static_cast<double>(strategy.processor.displayLapEntries)));
        strategyProcessor.Set("display_history_bytes", Napi::Number::New(info.Env(), static_cast<double>(strategy.processor.displayHistoryBytes)));
        strategyStats.Set("processor", strategyProcessor);
        result.Set("strategy", strategyStats);

        Napi::Object liveHistory = Napi::Object::New(info.Env());
        liveHistory.Set("retained_bytes", Napi::Number::New(info.Env(), static_cast<double>(history.retainedBytes)));
        liveHistory.Set("estimate_basis", Napi::String::New(info.Env(),
            "allocated vector/string capacities; transient compression/decompression activity is reported separately; lap/map/shared_ptr overhead excluded"));
        liveHistory.Set("laps", Napi::Number::New(info.Env(), static_cast<double>(history.lapCount)));
        liveHistory.Set("pinned_laps", Napi::Number::New(info.Env(), static_cast<double>(history.pinnedLapCount)));
        liveHistory.Set("compressed_laps", Napi::Number::New(info.Env(), static_cast<double>(history.compressedLapCount)));
        liveHistory.Set("busy_laps", Napi::Number::New(info.Env(), static_cast<double>(history.busyLapCount)));
        liveHistory.Set("packed_bytes", Napi::Number::New(info.Env(), static_cast<double>(history.packedBytes)));
        liveHistory.Set("packed_capacity_bytes", Napi::Number::New(info.Env(), static_cast<double>(history.packedCapacityBytes)));
        liveHistory.Set("json_rows", Napi::Number::New(info.Env(), static_cast<double>(history.jsonRows)));
        liveHistory.Set("json_payload_bytes", Napi::Number::New(info.Env(), static_cast<double>(history.jsonPayloadBytes)));
        liveHistory.Set("json_payload_capacity_bytes", Napi::Number::New(info.Env(), static_cast<double>(history.jsonPayloadCapacityBytes)));
        liveHistory.Set("json_container_capacity_bytes", Napi::Number::New(info.Env(), static_cast<double>(history.jsonContainerCapacityBytes)));
        liveHistory.Set("sequence_entries", Napi::Number::New(info.Env(), static_cast<double>(history.sequenceEntries)));
        liveHistory.Set("sequence_capacity_bytes", Napi::Number::New(info.Env(), static_cast<double>(history.sequenceCapacityBytes)));
        liveHistory.Set("compressed_plain_bytes", Napi::Number::New(info.Env(), static_cast<double>(history.compressedPlainBytes)));
        liveHistory.Set("compressed_payload_bytes", Napi::Number::New(info.Env(), static_cast<double>(history.compressedBytes)));
        liveHistory.Set("compressed_capacity_bytes", Napi::Number::New(info.Env(), static_cast<double>(history.compressedCapacityBytes)));
        liveHistory.Set("queued_jobs", Napi::Number::New(info.Env(), static_cast<double>(history.queuedJobs)));
        liveHistory.Set("active_job_kind", Napi::Number::New(info.Env(), history.activeJobKind));
        Napi::Object historyCompressionActivity = Napi::Object::New(info.Env());
        historyCompressionActivity.Set("jobs", Napi::Number::New(info.Env(), static_cast<double>(history.compressionJobs)));
        historyCompressionActivity.Set("families", Napi::Number::New(info.Env(), static_cast<double>(history.compressedFamilies)));
        historyCompressionActivity.Set("plain_bytes_processed", Napi::Number::New(info.Env(), static_cast<double>(history.compressionPlainBytesProcessed)));
        historyCompressionActivity.Set("plain_buffer_bytes_allocated", Napi::Number::New(info.Env(), static_cast<double>(history.compressionPlainBufferBytesAllocated)));
        historyCompressionActivity.Set("buffer_bytes_allocated", Napi::Number::New(info.Env(), static_cast<double>(history.compressionBufferBytesAllocated)));
        historyCompressionActivity.Set("output_bytes_allocated", Napi::Number::New(info.Env(), static_cast<double>(history.compressedOutputBytesAllocated)));
        historyCompressionActivity.Set("last_plain_bytes", Napi::Number::New(info.Env(), static_cast<double>(history.lastCompressionPlainBytes)));
        historyCompressionActivity.Set("last_buffer_bytes", Napi::Number::New(info.Env(), static_cast<double>(history.lastCompressionBufferBytes)));
        historyCompressionActivity.Set("last_scratch_bytes", Napi::Number::New(info.Env(), static_cast<double>(history.lastCompressionScratchBytes)));
        historyCompressionActivity.Set("peak_plain_bytes", Napi::Number::New(info.Env(), static_cast<double>(history.peakCompressionPlainBytes)));
        historyCompressionActivity.Set("peak_buffer_bytes", Napi::Number::New(info.Env(), static_cast<double>(history.peakCompressionBufferBytes)));
        historyCompressionActivity.Set("peak_scratch_bytes", Napi::Number::New(info.Env(), static_cast<double>(history.peakCompressionScratchBytes)));
        liveHistory.Set("compression_activity", historyCompressionActivity);
        Napi::Object historyDecompressionActivity = Napi::Object::New(info.Env());
        historyDecompressionActivity.Set("jobs", Napi::Number::New(info.Env(), static_cast<double>(history.decompressionJobs)));
        historyDecompressionActivity.Set("buffer_bytes_allocated", Napi::Number::New(info.Env(), static_cast<double>(history.decompressionBufferBytesAllocated)));
        historyDecompressionActivity.Set("last_buffer_bytes", Napi::Number::New(info.Env(), static_cast<double>(history.lastDecompressionBufferBytes)));
        historyDecompressionActivity.Set("peak_buffer_bytes", Napi::Number::New(info.Env(), static_cast<double>(history.peakDecompressionBufferBytes)));
        liveHistory.Set("decompression_activity", historyDecompressionActivity);
        liveHistory.Set("range_jobs", Napi::Number::New(info.Env(), static_cast<double>(history.rangeJobs)));
        result.Set("live_history", liveHistory);

        Napi::Object writerStats = Napi::Object::New(info.Env());
        writerStats.Set("stream_active", Napi::Boolean::New(info.Env(), writer.streamActive));
        writerStats.Set("retained_bytes", Napi::Number::New(info.Env(), static_cast<double>(writer.retainedBytes)));
        writerStats.Set("estimate_basis", Napi::String::New(info.Env(),
            "queued event object/payload allocation plus rolling, dedupe, V5 builder, reusable compression scratch, chunk-index, branch, event, and lap-status capacities; transient activity is reported separately; allocator and map/deque node overhead excluded"));
        writerStats.Set("queued_events", Napi::Number::New(info.Env(), static_cast<double>(writer.queuedEvents)));
        writerStats.Set("queued_retained_bytes", Napi::Number::New(info.Env(), static_cast<double>(writer.queuedRetainedBytes)));
        writerStats.Set("queued_record_events", Napi::Number::New(info.Env(), static_cast<double>(writer.queuedRecordEvents)));
        writerStats.Set("queued_note_packet_events", Napi::Number::New(info.Env(), static_cast<double>(writer.queuedNotePacketEvents)));
        writerStats.Set("queued_control_events", Napi::Number::New(info.Env(), static_cast<double>(writer.queuedControlEvents)));
        writerStats.Set("queued_json_bytes", Napi::Number::New(info.Env(), static_cast<double>(writer.queuedJsonBytes)));
        writerStats.Set("queued_packet_bytes", Napi::Number::New(info.Env(), static_cast<double>(writer.queuedPacketBytes)));
        writerStats.Set("oldest_queued_event_age_ms", Napi::Number::New(info.Env(), static_cast<double>(writer.oldestQueuedEventAgeMs)));

        Napi::Object rolling = Napi::Object::New(info.Env());
        rolling.Set("entries", Napi::Number::New(info.Env(), static_cast<double>(writer.rollingEntries)));
        rolling.Set("payload_bytes", Napi::Number::New(info.Env(), static_cast<double>(writer.rollingPayloadBytes)));
        rolling.Set("payload_capacity_bytes", Napi::Number::New(info.Env(), static_cast<double>(writer.rollingPayloadCapacityBytes)));
        rolling.Set("container_capacity_bytes", Napi::Number::New(info.Env(), static_cast<double>(writer.rollingContainerCapacityBytes)));
        Napi::Object rollingActivity = Napi::Object::New(info.Env());
        rollingActivity.Set("batches", Napi::Number::New(info.Env(), static_cast<double>(writer.rollingFlushBatches)));
        rollingActivity.Set("entries_processed", Napi::Number::New(info.Env(), static_cast<double>(writer.rollingFlushEntriesProcessed)));
        rollingActivity.Set("payload_bytes_processed", Napi::Number::New(info.Env(), static_cast<double>(writer.rollingFlushPayloadBytesProcessed)));
        rollingActivity.Set("last_entries", Napi::Number::New(info.Env(), static_cast<double>(writer.lastRollingFlushEntries)));
        rollingActivity.Set("last_payload_bytes", Napi::Number::New(info.Env(), static_cast<double>(writer.lastRollingFlushPayloadBytes)));
        rollingActivity.Set("last_copy_capacity_bytes", Napi::Number::New(info.Env(), static_cast<double>(writer.lastRollingFlushCopyCapacityBytes)));
        rollingActivity.Set("peak_entries", Napi::Number::New(info.Env(), static_cast<double>(writer.peakRollingFlushEntries)));
        rollingActivity.Set("peak_payload_bytes", Napi::Number::New(info.Env(), static_cast<double>(writer.peakRollingFlushPayloadBytes)));
        rollingActivity.Set("peak_copy_capacity_bytes", Napi::Number::New(info.Env(), static_cast<double>(writer.peakRollingFlushCopyCapacityBytes)));
        rolling.Set("flush_copy_activity", rollingActivity);
        writerStats.Set("rolling_buffer", rolling);

        Napi::Object appendActivity = Napi::Object::New(info.Env());
        appendActivity.Set("batches", Napi::Number::New(info.Env(), static_cast<double>(writer.v5AppendBatches)));
        appendActivity.Set("rows_processed", Napi::Number::New(info.Env(), static_cast<double>(writer.v5AppendRowsProcessed)));
        appendActivity.Set("payload_bytes_processed", Napi::Number::New(info.Env(), static_cast<double>(writer.v5AppendPayloadBytesProcessed)));
        appendActivity.Set("last_rows", Napi::Number::New(info.Env(), static_cast<double>(writer.lastV5AppendRows)));
        appendActivity.Set("last_payload_bytes", Napi::Number::New(info.Env(), static_cast<double>(writer.lastV5AppendPayloadBytes)));
        appendActivity.Set("last_source_row_capacity_bytes", Napi::Number::New(info.Env(), static_cast<double>(writer.lastV5SourceRowCapacityBytes)));
        appendActivity.Set("peak_rows", Napi::Number::New(info.Env(), static_cast<double>(writer.peakV5AppendRows)));
        appendActivity.Set("peak_payload_bytes", Napi::Number::New(info.Env(), static_cast<double>(writer.peakV5AppendPayloadBytes)));
        appendActivity.Set("peak_source_row_capacity_bytes", Napi::Number::New(info.Env(), static_cast<double>(writer.peakV5SourceRowCapacityBytes)));
        writerStats.Set("v5_append_activity", appendActivity);

        Napi::Object dedupe = Napi::Object::New(info.Env());
        dedupe.Set("entries", Napi::Number::New(info.Env(), static_cast<double>(writer.dedupeEntries)));
        dedupe.Set("payload_bytes", Napi::Number::New(info.Env(), static_cast<double>(writer.dedupePayloadBytes)));
        dedupe.Set("payload_capacity_bytes", Napi::Number::New(info.Env(), static_cast<double>(writer.dedupePayloadCapacityBytes)));
        writerStats.Set("dedupe_cache", dedupe);

        Napi::Object v5 = Napi::Object::New(info.Env());
        v5.Set("retained_bytes", Napi::Number::New(info.Env(), static_cast<double>(writer.v5RetainedBytes)));
        v5.Set("builders", Napi::Number::New(info.Env(), static_cast<double>(writer.v5BuilderCount)));
        v5.Set("builder_plain_bytes", Napi::Number::New(info.Env(), static_cast<double>(writer.v5BuilderPlainBytes)));
        v5.Set("builder_plain_capacity_bytes", Napi::Number::New(info.Env(), static_cast<double>(writer.v5BuilderPlainCapacityBytes)));
        v5.Set("builder_row_index_entries", Napi::Number::New(info.Env(), static_cast<double>(writer.v5BuilderRowIndexEntries)));
        v5.Set("builder_row_index_capacity_bytes", Napi::Number::New(info.Env(), static_cast<double>(writer.v5BuilderRowIndexCapacityBytes)));
        v5.Set("chunks", Napi::Number::New(info.Env(), static_cast<double>(writer.v5ChunkCount)));
        v5.Set("chunk_container_capacity_bytes", Napi::Number::New(info.Env(), static_cast<double>(writer.v5ChunkContainerCapacityBytes)));
        v5.Set("chunk_row_index_entries", Napi::Number::New(info.Env(), static_cast<double>(writer.v5ChunkRowIndexEntries)));
        v5.Set("chunk_row_index_capacity_bytes", Napi::Number::New(info.Env(), static_cast<double>(writer.v5ChunkRowIndexCapacityBytes)));
        v5.Set("branches", Napi::Number::New(info.Env(), static_cast<double>(writer.v5BranchCount)));
        v5.Set("branch_capacity_bytes", Napi::Number::New(info.Env(), static_cast<double>(writer.v5BranchCapacityBytes)));
        v5.Set("laps", Napi::Number::New(info.Env(), static_cast<double>(writer.v5LapCount)));
        v5.Set("status_laps", Napi::Number::New(info.Env(), static_cast<double>(writer.v5StatusLapCount)));
        v5.Set("events", Napi::Number::New(info.Env(), static_cast<double>(writer.v5EventCount)));
        v5.Set("event_payload_bytes", Napi::Number::New(info.Env(), static_cast<double>(writer.v5EventPayloadBytes)));
        v5.Set("event_payload_capacity_bytes", Napi::Number::New(info.Env(), static_cast<double>(writer.v5EventPayloadCapacityBytes)));
        v5.Set("event_container_capacity_bytes", Napi::Number::New(info.Env(), static_cast<double>(writer.v5EventContainerCapacityBytes)));
        v5.Set("lap_status_capacity_bytes", Napi::Number::New(info.Env(), static_cast<double>(writer.v5LapStatusCapacityBytes)));
        Napi::Object compressionActivity = Napi::Object::New(info.Env());
        compressionActivity.Set("chunk_writes", Napi::Number::New(info.Env(), static_cast<double>(writer.v5ChunkWrites)));
        compressionActivity.Set("plain_bytes_processed", Napi::Number::New(info.Env(), static_cast<double>(writer.v5ChunkPlainBytesProcessed)));
        compressionActivity.Set("compressed_bytes_written", Napi::Number::New(info.Env(), static_cast<double>(writer.v5ChunkCompressedBytesWritten)));
        compressionActivity.Set("buffer_bytes_allocated", Napi::Number::New(info.Env(), static_cast<double>(writer.v5CompressionBufferBytesAllocated)));
        compressionActivity.Set("retained_scratch_capacity_bytes", Napi::Number::New(info.Env(), static_cast<double>(writer.v5CompressionScratchCapacityBytes)));
        compressionActivity.Set("retained_context_bytes", Napi::Number::New(info.Env(), static_cast<double>(writer.v5CompressionContextBytes)));
        compressionActivity.Set("last_plain_bytes", Napi::Number::New(info.Env(), static_cast<double>(writer.v5LastChunkPlainBytes)));
        compressionActivity.Set("last_compressed_bytes", Napi::Number::New(info.Env(), static_cast<double>(writer.v5LastChunkCompressedBytes)));
        compressionActivity.Set("last_buffer_capacity_bytes", Napi::Number::New(info.Env(), static_cast<double>(writer.v5LastCompressionBufferCapacityBytes)));
        compressionActivity.Set("peak_buffer_capacity_bytes", Napi::Number::New(info.Env(), static_cast<double>(writer.v5PeakCompressionBufferCapacityBytes)));
        v5.Set("compression_activity", compressionActivity);
        Napi::Object checkpointActivity = Napi::Object::New(info.Env());
        checkpointActivity.Set("writes", Napi::Number::New(info.Env(), static_cast<double>(writer.v5CheckpointWrites)));
        checkpointActivity.Set("scratch_bytes_allocated", Napi::Number::New(info.Env(), static_cast<double>(writer.v5CheckpointScratchBytesAllocated)));
        checkpointActivity.Set("last_scratch_bytes", Napi::Number::New(info.Env(), static_cast<double>(writer.v5LastCheckpointScratchBytes)));
        checkpointActivity.Set("peak_scratch_bytes", Napi::Number::New(info.Env(), static_cast<double>(writer.v5PeakCheckpointScratchBytes)));
        checkpointActivity.Set("last_directory_bytes", Napi::Number::New(info.Env(), static_cast<double>(writer.v5LastCheckpointDirectoryBytes)));
        checkpointActivity.Set("peak_directory_bytes", Napi::Number::New(info.Env(), static_cast<double>(writer.v5PeakCheckpointDirectoryBytes)));
        checkpointActivity.Set("last_row_index_bytes", Napi::Number::New(info.Env(), static_cast<double>(writer.v5LastCheckpointRowIndexBytes)));
        checkpointActivity.Set("peak_row_index_bytes", Napi::Number::New(info.Env(), static_cast<double>(writer.v5PeakCheckpointRowIndexBytes)));
        v5.Set("checkpoint_activity", checkpointActivity);
        writerStats.Set("v5", v5);
        result.Set("writer", writerStats);
        return result;
    }

    Napi::Value SetTeamColorOverrides(const Napi::CallbackInfo& info) {
        if (engine && info.Length() >= 1)
            engine->setTeamColorOverrides(readTeamColorOverrides(info[0]));
        return info.Env().Undefined();
    }

    Napi::Value TeamColorCatalog(const Napi::CallbackInfo& info) {
        return Napi::String::New(info.Env(), engine
            ? engine->teamColorCatalogJson() : "{}");
    }

    Napi::Value SetStrategyMinimumStops(const Napi::CallbackInfo& info) {
        if (info.Length() >= 1 && info[0].IsNumber())
            engine->setStrategyMinimumStops(info[0].As<Napi::Number>().Int32Value());
        return info.Env().Undefined();
    }

    Napi::Value SetLogging(const Napi::CallbackInfo& info) {
        return SetLoggingZstd(info);
    }

    Napi::Value FlushRecording(const Napi::CallbackInfo& info) {
        if (engine) engine->flushRecording();
        return info.Env().Undefined();
    }

    Napi::Value SetLoggingZstd(const Napi::CallbackInfo& info) {
        if (info.Length() >= 2 && info[0].IsBoolean() && info[1].IsString()) {
            bool enabled = info[0].As<Napi::Boolean>().Value();
            std::string dir = info[1].As<Napi::String>().Utf8Value();
            engine->setLoggingZstd(enabled, dir);
        }
        return info.Env().Undefined();
    }

    // Deprecated compatibility surface. Normal app recording never calls it.
    Napi::Value SetLoggingGzip(const Napi::CallbackInfo& info) {
        if (info.Length() >= 2 && info[0].IsBoolean() && info[1].IsString()) {
            bool enabled = info[0].As<Napi::Boolean>().Value();
            std::string dir = info[1].As<Napi::String>().Utf8Value();
#if defined(__clang__) || defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#elif defined(_MSC_VER)
#  pragma warning(push)
#  pragma warning(disable: 4996)
#endif
            engine->setLoggingGzip(enabled, dir);
#if defined(__clang__) || defined(__GNUC__)
#  pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#  pragma warning(pop)
#endif
        }
        return info.Env().Undefined();
    }

    // Async: resolves Promise<{ok, error?}>. The load (decompress + index scan) runs
    // on the libuv threadpool so the Electron main thread stays responsive.
    Napi::Value PlayerLoad(const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();
        auto resolveFailure = [&env](const char* error) {
            auto d = Napi::Promise::Deferred::New(env);
            Napi::Object result = Napi::Object::New(env);
            result.Set("ok", false);
            result.Set("error", error);
            d.Resolve(result);
            return d.Promise();
        };
        if (info.Length() < 1 || !info[0].IsString() || !engine)
            return resolveFailure("The playback engine is not available.");
        if (loadBusy_->exchange(true))
            return resolveFailure("Another recording is already being opened.");
        auto* worker = new PlayerLoadWorker(env, engine,
                                            info[0].As<Napi::String>().Utf8Value(), loadBusy_);
        Napi::Promise p = worker->GetPromise();
        worker->Queue();
        return p;
    }

    Napi::Value PlayerPlay(const Napi::CallbackInfo& info) {
        engine->playerPlay();
        return info.Env().Undefined();
    }

    Napi::Value PlayerPause(const Napi::CallbackInfo& info) {
        engine->playerPause();
        return info.Env().Undefined();
    }

    Napi::Value SetDataRequirements(const Napi::CallbackInfo& info) {
        if (engine && info.Length() >= 2 && info[0].IsNumber() &&
            info[1].IsNumber()) {
            const uint32_t streamMask = info[0].As<Napi::Number>().Uint32Value();
            const uint32_t historyMask = info[1].As<Napi::Number>().Uint32Value();
            const float windowSeconds = info.Length() >= 3 && info[2].IsNumber()
                ? std::max(-1.0f, info[2].As<Napi::Number>().FloatValue()) : 0.0f;
            const uint64_t requestId = info.Length() >= 4 && info[3].IsNumber()
                ? static_cast<uint64_t>(info[3].As<Napi::Number>().Int64Value()) : 0;
            engine->requestDataRequirements(requestId);
            (new DataRequirementsWorker(info.Env(), engine, streamMask,
                historyMask, windowSeconds, requestId))->Queue();
        }
        return info.Env().Undefined();
    }

    Napi::Value PlayerSeek(const Napi::CallbackInfo& info) {
        if (info.Length() >= 1 && info[0].IsNumber()) {
            const bool allHistory = info.Length() >= 2 && info[1].IsBoolean() &&
                info[1].As<Napi::Boolean>().Value();
            const uint64_t requestId = info.Length() >= 3 && info[2].IsNumber()
                ? static_cast<uint64_t>(info[2].As<Napi::Number>().Int64Value()) : 0;
            const uint32_t rowTypeMask = info.Length() >= 4 && info[3].IsNumber()
                ? info[3].As<Napi::Number>().Uint32Value() : 0xFFFFFFFFu;
            const float windowSeconds = info.Length() >= 5 && info[4].IsNumber()
                ? std::max(0.0f, info[4].As<Napi::Number>().FloatValue()) : 0.0f;
            engine->playerRequestSeek(requestId);
            // Every indexed seek can decompress, rebuild sparse state and prime
            // the streaming frontier. Never charge that work to Electron's
            // main thread, including current-lap/zero-window seeks.
            (new PlayerSeekWorker(info.Env(), engine,
                info[0].As<Napi::Number>().FloatValue(), allHistory, requestId,
                rowTypeMask, windowSeconds))->Queue();
        }
        return info.Env().Undefined();
    }

    Napi::Value PlayerSetSpeed(const Napi::CallbackInfo& info) {
        if (info.Length() >= 1 && info[0].IsNumber()) {
            engine->playerSetSpeed(info[0].As<Napi::Number>().FloatValue());
        }
        return info.Env().Undefined();
    }

    Napi::Value LiveGetFastestLap(const Napi::CallbackInfo& info) {
        if (engine && info.Length() >= 1 && info[0].IsNumber())
            engine->liveGetFastestLap(static_cast<uint64_t>(info[0].As<Napi::Number>().Int64Value()));
        return info.Env().Undefined();
    }

    Napi::Value PlayerGetLapData(const Napi::CallbackInfo& info) {
        if (info.Length() >= 1 && info[0].IsNumber() && engine) {
            const uint32_t rowTypeMask = info.Length() >= 2 && info[1].IsNumber()
                ? info[1].As<Napi::Number>().Uint32Value() : 0xFFFFFFFFu;
            engine->playerGetLapData(info[0].As<Napi::Number>().Int32Value(), rowTypeMask);
        }
        return info.Env().Undefined();
    }

    Napi::Value PlayerGetAllLapsData(const Napi::CallbackInfo& info) {
        if (engine) {
            const uint64_t requestId = info.Length() >= 1 && info[0].IsNumber()
                ? static_cast<uint64_t>(info[0].As<Napi::Number>().Int64Value()) : 0;
            const uint32_t rowTypeMask = info.Length() >= 2 && info[1].IsNumber()
                ? info[1].As<Napi::Number>().Uint32Value() : 0xFFFFFFFFu;
            (new PlayerHistoryWorker(info.Env(), engine, requestId, rowTypeMask))->Queue();
        }
        return info.Env().Undefined();
    }

    Napi::Value PlayerGetWindowData(const Napi::CallbackInfo& info) {
        if (engine && info.Length() >= 1 && info[0].IsNumber()) {
            const float windowSeconds = std::max(0.0f, info[0].As<Napi::Number>().FloatValue());
            const uint64_t requestId = info.Length() >= 2 && info[1].IsNumber()
                ? static_cast<uint64_t>(info[1].As<Napi::Number>().Int64Value()) : 0;
            const uint32_t rowTypeMask = info.Length() >= 3 && info[2].IsNumber()
                ? info[2].As<Napi::Number>().Uint32Value() : 0xFFFFFFFFu;
            (new PlayerHistoryWorker(info.Env(), engine, requestId, rowTypeMask,
                                     windowSeconds, true))->Queue();
        }
        return info.Env().Undefined();
    }

    Napi::Value AnalysisLoadFile(const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();
        auto resolveFailure = [&env](const char* error) {
            auto deferred = Napi::Promise::Deferred::New(env);
            Napi::Object result = Napi::Object::New(env);
            result.Set("ok", false);
            result.Set("error", error);
            deferred.Resolve(result);
            return deferred.Promise();
        };
        if (info.Length() < 1 || !info[0].IsString())
            return resolveFailure("A recording path is required.");
        if (analysisReader_->busy.exchange(true))
            return resolveFailure("Another comparison recording is already being opened.");

        auto* worker = new AnalysisLoadWorker(
            env, analysisReader_, info[0].As<Napi::String>().Utf8Value());
        Napi::Promise promise = worker->GetPromise();
        worker->Queue();
        return promise;
    }

    Napi::Value AnalysisGetLapData(const Napi::CallbackInfo& info) {
        if (info.Length() < 1 || !info[0].IsNumber() || analysisReader_->busy.load())
            return Napi::String::New(info.Env(), "");
        std::lock_guard<std::mutex> lock(analysisReader_->mutex);
        return Napi::String::New(
            info.Env(), analysisReader_->reader.getLapDataMessage(
                info[0].As<Napi::Number>().Int32Value(),
                info.Length() >= 2 && info[1].IsNumber()
                    ? info[1].As<Napi::Number>().Uint32Value() : 0xFFFFFFFFu));
    }

    Napi::Value AnalysisCompareLaps(const Napi::CallbackInfo& info) {
        auto resolveEmpty = [&info]() {
            auto deferred = Napi::Promise::Deferred::New(info.Env());
            deferred.Resolve(Napi::String::New(info.Env(), ""));
            return deferred.Promise();
        };
        if (info.Length() < 5 || !info[0].IsNumber() || !info[1].IsBoolean() ||
            !info[2].IsNumber() || !info[3].IsBoolean() || !info[4].IsBoolean() ||
            !engine || analysisReader_->busy.load()) return resolveEmpty();

        auto* worker = new AnalysisDeltaWorker(
            info.Env(), engine, analysisReader_,
            info[0].As<Napi::Number>().Int32Value(), info[1].As<Napi::Boolean>().Value(),
            info[2].As<Napi::Number>().Int32Value(), info[3].As<Napi::Boolean>().Value(),
            info[4].As<Napi::Boolean>().Value());
        Napi::Promise promise = worker->GetPromise();
        worker->Queue();
        return promise;
    }

    Napi::Value AnalysisCloseFile(const Napi::CallbackInfo& info) {
        if (!analysisReader_->busy.load()) {
            std::lock_guard<std::mutex> lock(analysisReader_->mutex);
            analysisReader_->reader.close();
        }
        return info.Env().Undefined();
    }

    Napi::Value Destroy(const Napi::CallbackInfo& info) {
        if (destroyed_) return info.Env().Undefined();
        destroyed_ = true;
        if (engine) engine->pairStop(false);
        engine.reset();   // a pending PlayerLoadWorker holds its own ref
        tsfn.Release();
        if (hasBinCb_) tsfnBin.Release();
        if (hasSeekCb_) tsfnSeek.Release();
        if (hasPairCb_) tsfnPair.Release();
        if (hasPairDiagnosticCb_) tsfnPairDiagnostic.Release();
        return info.Env().Undefined();
    }

    Napi::Value PlayerClose(const Napi::CallbackInfo& info) {
        std::fprintf(stderr, "[close-trace] addon PlayerClose entry\n");
        std::fflush(stderr);
        // JSON control rows and binary hot rows use separate thread-safe
        // callbacks. Invalidate the playback binary callback before stopping
        // the engine so it cannot run after playback_close and repopulate the
        // renderer with the final speed/RPM/temperature batch. While the close
        // is in progress UDP is either rejected by the engine or discarded
        // here; new live rows use the next generation after this call returns.
        auto binaryFlush = binFlush_;
        {
            std::lock_guard<std::mutex> lock(binaryFlush->mutex);
            binaryFlush->discard = true;
            ++binaryFlush->generation;
            binaryFlush->pending.clear();
            binaryFlush->draining.clear();
            binaryFlush->scheduled = false;
            binaryFlush->scheduledGeneration = binaryFlush->generation;
        }
        if (engine) engine->playerClose();
        {
            std::lock_guard<std::mutex> lock(binaryFlush->mutex);
            binaryFlush->discard = false;
        }
        std::fprintf(stderr, "[close-trace] addon PlayerClose returned\n");
        std::fflush(stderr);
        return info.Env().Undefined();
    }

    // Exports an arbitrary .tnrd file to XLSX. Deliberately independent of
    // `engine`'s playback state — the Electron app's playback path
    // (src/main/sessionPlayer.ts) doesn't use this native Engine's
    // player*/TnrdReader at all, so this opens its own throwaway TnrdReader
    // internally (see tnrp::exportTnrdFileToXlsx). Registered as an instance
    // method purely because that's where N-API methods live on this class.
    Napi::Value PlayerExportXlsx(const Napi::CallbackInfo& info) {
        Napi::Env env = info.Env();
        if (info.Length() < 3 || !info[0].IsString() || !info[1].IsString() || !info[2].IsFunction()) {
            Napi::TypeError::New(env, "Expected (srcPath, destPath, onProgress)").ThrowAsJavaScriptException();
            return env.Undefined();
        }
        auto* worker = new ExportXlsxWorker(env, info[0].As<Napi::String>().Utf8Value(),
                                                  info[1].As<Napi::String>().Utf8Value(),
                                                  info[2].As<Napi::Function>());
        Napi::Promise p = worker->GetPromise();
        worker->Queue();
        return p;
    }
};

// Module-level: the i18n label catalog JSON for a given packet format
// (2024/2025/2026). Lets the TS playback path emit a protocol_status with the
// recorded clip's labels without going through an Engine instance.
Napi::Value LabelsJson(const Napi::CallbackInfo& info) {
    Napi::Env env = info.Env();
    uint16_t format = 2025;
    if (info.Length() >= 1 && info[0].IsNumber())
        format = static_cast<uint16_t>(info[0].As<Napi::Number>().Uint32Value());
    return Napi::String::New(env, tnrp::labelsJson(format));
}

// Module-level: the declarative card-colour spec (format-independent JSON). Lets
// both the live and playback paths hand the renderer one shared colour model.
Napi::Value CardColorsJson(const Napi::CallbackInfo& info) {
    return Napi::String::New(info.Env(), tnrp::cardColorsJson());
}

// Module-level: reclaim stale "tracknrace_*.tmp" decompression temps left by
// crashed runs (either app). Call once at startup, after the single-instance
// lock is held, so a second instance can't unlink an active session's temp.
Napi::Value SweepTempFiles(const Napi::CallbackInfo& info) {
    tnrp::TnrdReader::sweepStaleTempFiles();
    return info.Env().Undefined();
}

Napi::Object InitAll(Napi::Env env, Napi::Object exports) {
    TRACE("InitAll: module loading");
    TNRPAddon::Init(env, exports);
    TRACE("InitAll: module loaded");
    exports.Set("labelsJson", Napi::Function::New(env, LabelsJson));
    exports.Set("cardColorsJson", Napi::Function::New(env, CardColorsJson));
    exports.Set("sweepTempFiles", Napi::Function::New(env, SweepTempFiles));
    return exports;
}

NODE_API_MODULE(protocol_parser, InitAll)
