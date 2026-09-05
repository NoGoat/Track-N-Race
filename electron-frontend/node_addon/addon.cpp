#include <napi.h>
#include <tnrp/Engine.h>
#include <tnrp/Labels.h>
#include <tnrp/CardColors.h>
#include <tnrp/TnrdReader.h>
#include <tnrp/XlsxExport.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using namespace Napi;

#define TRACE(msg) do { fprintf(stderr, "[native] " msg "\n"); fflush(stderr); } while (0)

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

class TNRPAddon : public Napi::ObjectWrap<TNRPAddon>, public tnrp::Sink {
public:
    static Napi::Object Init(Napi::Env env, Napi::Object exports) {
        TRACE("Init: before DefineClass");
        Napi::Function func = DefineClass(env, "Engine", {
            InstanceMethod("startUdp", &TNRPAddon::StartUdp),
            InstanceMethod("udpLastError", &TNRPAddon::UdpLastError),
            InstanceMethod("liveDiagnostics", &TNRPAddon::LiveDiagnostics),
            InstanceMethod("setOverride", &TNRPAddon::SetOverride),
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
            InstanceMethod("playerGetAllLapsData", &TNRPAddon::PlayerGetAllLapsData),
            InstanceMethod("playerGetWindowData", &TNRPAddon::PlayerGetWindowData),
            InstanceMethod("playerClose", &TNRPAddon::PlayerClose),
            InstanceMethod("analysisLoadFile", &TNRPAddon::AnalysisLoadFile),
            InstanceMethod("analysisGetLapData", &TNRPAddon::AnalysisGetLapData),
            InstanceMethod("analysisCloseFile", &TNRPAddon::AnalysisCloseFile),
            InstanceMethod("playerExportXlsx", &TNRPAddon::PlayerExportXlsx),
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
        TRACE("TNRPAddon ctor: about to construct Engine");

        engine = std::make_shared<tnrp::Engine>(config, this);
        TRACE("TNRPAddon ctor: Engine constructed, done");
    }

    ~TNRPAddon() {
        if (!destroyed_) {
            if (engine) engine->playerClose();
            tsfn.Release();
            if (hasBinCb_) tsfnBin.Release();
            if (hasSeekCb_) tsfnSeek.Release();
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
            if (!fs->scheduled) { fs->scheduled = true; schedule = true; }
        }
        if (!schedule) return;

        auto status = tsfn.NonBlockingCall([fs](Napi::Env env, Napi::Function cb) {
            {
                std::lock_guard<std::mutex> lk(fs->mutex);
                fs->draining.swap(fs->pending);  // grab the batch; pending keeps reusable storage
                fs->scheduled = false;
            }
            if (env != nullptr && cb != nullptr) {
                cb.Call({ Napi::String::New(env, fs->draining) });
            }
            std::lock_guard<std::mutex> lk(fs->mutex);
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
        {
            std::lock_guard<std::mutex> lk(fs->mutex);
            fs->pending.insert(fs->pending.end(), data, data + len);
            if (!fs->scheduled) { fs->scheduled = true; schedule = true; }
        }
        if (!schedule) return;

        auto status = tsfnBin.NonBlockingCall([fs](Napi::Env env, Napi::Function cb) {
            {
                std::lock_guard<std::mutex> lk(fs->mutex);
                fs->draining.swap(fs->pending);  // grab the batch; pending keeps storage
                fs->scheduled = false;
            }
            if (env != nullptr && cb != nullptr) {
                cb.Call({ Napi::Buffer<uint8_t>::Copy(env, fs->draining.data(), fs->draining.size()) });
            }
            std::lock_guard<std::mutex> lk(fs->mutex);
            fs->draining.clear();                // retain capacity for the next swap
        });

        if (status != napi_ok) {
            std::lock_guard<std::mutex> lk(fs->mutex);
            fs->scheduled = false;
        }
    }

    // Playback seek flush (Config::binaryPlayback only). The reader hands us a
    // zero-copy view of its immutable packed store. Copy once into a normal V8
    // Buffer here: Electron IPC cannot serialize an external-memory Buffer.
    void onSeekFlush(std::shared_ptr<const std::vector<uint8_t>> binStore,
                     size_t binBegin, size_t binEnd, std::string&& coldJson,
                     float currentLapStart, int lapNum, bool allHistory,
                     uint64_t requestId, bool authoritativeSeek,
                     uint32_t rowTypeMask) override {
        if (!hasSeekCb_) return;
        struct SeekData {
            std::shared_ptr<const std::vector<uint8_t>> binStore;
            size_t binBegin;
            size_t binEnd;
            std::string cold;
            float lapStart;
            int lapNum;
            bool allHistory;
            uint64_t requestId;
            bool authoritativeSeek;
            uint32_t rowTypeMask;
        };
        auto* d = new SeekData{ std::move(binStore), binBegin, binEnd, std::move(coldJson),
                                currentLapStart, lapNum, allHistory, requestId,
                                authoritativeSeek, rowTypeMask };
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
                              Napi::Number::New(env, d->rowTypeMask) });
                }
                delete d;
            });
        if (status != napi_ok) delete d;
    }

private:
    // Shared so queued flush callbacks remain valid even if the wrapper is torn down.
    struct FlushState {
        std::mutex  mutex;
        std::string pending;    // newline-delimited JSON awaiting delivery
        std::string draining;   // batch currently being handed to JS (reused storage)
        bool        scheduled = false;
    };

    // Shared so queued binary flush callbacks remain valid past teardown.
    struct BinFlushState {
        std::mutex           mutex;
        std::vector<uint8_t> pending;    // bytes awaiting delivery
        std::vector<uint8_t> draining;   // batch currently handed to JS (reused storage)
        bool                 scheduled = false;
    };

    std::shared_ptr<tnrp::Engine> engine;
    Napi::ThreadSafeFunction tsfn;
    Napi::ThreadSafeFunction tsfnBin;
    Napi::ThreadSafeFunction tsfnSeek;
    bool destroyed_ = false;
    bool hasBinCb_  = false;
    bool hasSeekCb_ = false;
    std::shared_ptr<FlushState>    flush_    = std::make_shared<FlushState>();
    std::shared_ptr<BinFlushState> binFlush_ = std::make_shared<BinFlushState>();
    std::shared_ptr<std::atomic<bool>> loadBusy_ = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<AnalysisReaderState> analysisReader_ = std::make_shared<AnalysisReaderState>();

    Napi::Value StartUdp(const Napi::CallbackInfo& info) {
        TRACE("StartUdp: calling engine->startUdp()");
        bool ok = engine->startUdp();
        TRACE("StartUdp: returned");
        return Napi::Boolean::New(info.Env(), ok);
    }

    Napi::Value UdpLastError(const Napi::CallbackInfo& info) {
        return Napi::String::New(info.Env(), engine->udpLastError());
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
            if (allHistory || windowSeconds > 0.0f) {
                (new PlayerSeekWorker(info.Env(), engine,
                    info[0].As<Napi::Number>().FloatValue(), allHistory, requestId,
                    rowTypeMask, windowSeconds))->Queue();
            } else {
                engine->playerSeek(info[0].As<Napi::Number>().FloatValue(), false,
                                   requestId, rowTypeMask, 0.0f);
            }
        }
        return info.Env().Undefined();
    }

    Napi::Value PlayerSetSpeed(const Napi::CallbackInfo& info) {
        if (info.Length() >= 1 && info[0].IsNumber()) {
            engine->playerSetSpeed(info[0].As<Napi::Number>().FloatValue());
        }
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
        engine.reset();   // a pending PlayerLoadWorker holds its own ref
        tsfn.Release();
        if (hasBinCb_) tsfnBin.Release();
        if (hasSeekCb_) tsfnSeek.Release();
        return info.Env().Undefined();
    }

    Napi::Value PlayerClose(const Napi::CallbackInfo& info) {
        if (engine) engine->playerClose();
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
