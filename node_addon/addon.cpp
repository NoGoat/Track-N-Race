#include <napi.h>
#include <tnrp/Engine.h>
#include <tnrp/Labels.h>
#include <tnrp/CardColors.h>
#include <tnrp/XlsxExport.h>
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

class TNRPAddon : public Napi::ObjectWrap<TNRPAddon>, public tnrp::Sink {
public:
    static Napi::Object Init(Napi::Env env, Napi::Object exports) {
        TRACE("Init: before DefineClass");
        Napi::Function func = DefineClass(env, "Engine", {
            InstanceMethod("startUdp", &TNRPAddon::StartUdp),
            InstanceMethod("setOverride", &TNRPAddon::SetOverride),
            InstanceMethod("setLogging", &TNRPAddon::SetLogging),
            InstanceMethod("playerLoad", &TNRPAddon::PlayerLoad),
            InstanceMethod("playerPlay", &TNRPAddon::PlayerPlay),
            InstanceMethod("playerPause", &TNRPAddon::PlayerPause),
            InstanceMethod("playerSeek", &TNRPAddon::PlayerSeek),
            InstanceMethod("playerSetSpeed", &TNRPAddon::PlayerSetSpeed),
            InstanceMethod("playerGetLapData", &TNRPAddon::PlayerGetLapData),
            InstanceMethod("playerClose", &TNRPAddon::PlayerClose),
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
            config.port = configObj.Get("port").As<Napi::Number>().Uint32Value();
        }
        if (configObj.Has("bindAddress") && configObj.Get("bindAddress").IsString()) {
            config.bindAddress = configObj.Get("bindAddress").As<Napi::String>().Utf8Value();
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
        TRACE("TNRPAddon ctor: about to construct Engine");

        engine = std::make_unique<tnrp::Engine>(config, this);
        TRACE("TNRPAddon ctor: Engine constructed, done");
    }

    ~TNRPAddon() {
        if (!destroyed_) {
            if (engine) engine->playerClose();
            tsfn.Release();
            if (hasBinCb_) tsfnBin.Release();
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

    std::unique_ptr<tnrp::Engine> engine;
    Napi::ThreadSafeFunction tsfn;
    Napi::ThreadSafeFunction tsfnBin;
    bool destroyed_ = false;
    bool hasBinCb_  = false;
    std::shared_ptr<FlushState>    flush_    = std::make_shared<FlushState>();
    std::shared_ptr<BinFlushState> binFlush_ = std::make_shared<BinFlushState>();

    Napi::Value StartUdp(const Napi::CallbackInfo& info) {
        TRACE("StartUdp: calling engine->startUdp()");
        bool ok = engine->startUdp();
        TRACE("StartUdp: returned");
        return Napi::Boolean::New(info.Env(), ok);
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
        if (info.Length() >= 2 && info[0].IsBoolean() && info[1].IsString()) {
            bool enabled = info[0].As<Napi::Boolean>().Value();
            std::string dir = info[1].As<Napi::String>().Utf8Value();
            engine->setLogging(enabled, dir);
        }
        return info.Env().Undefined();
    }

    Napi::Value PlayerLoad(const Napi::CallbackInfo& info) {
        if (info.Length() >= 1 && info[0].IsString()) {
            bool res = engine->playerLoad(info[0].As<Napi::String>().Utf8Value());
            return Napi::Boolean::New(info.Env(), res);
        }
        return Napi::Boolean::New(info.Env(), false);
    }

    Napi::Value PlayerPlay(const Napi::CallbackInfo& info) {
        engine->playerPlay();
        return info.Env().Undefined();
    }

    Napi::Value PlayerPause(const Napi::CallbackInfo& info) {
        engine->playerPause();
        return info.Env().Undefined();
    }

    Napi::Value PlayerSeek(const Napi::CallbackInfo& info) {
        if (info.Length() >= 1 && info[0].IsNumber()) {
            engine->playerSeek(info[0].As<Napi::Number>().FloatValue());
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
            engine->playerGetLapData(info[0].As<Napi::Number>().Int32Value());
        }
        return info.Env().Undefined();
    }

    Napi::Value Destroy(const Napi::CallbackInfo& info) {
        if (destroyed_) return info.Env().Undefined();
        destroyed_ = true;
        engine.reset();
        tsfn.Release();
        if (hasBinCb_) tsfnBin.Release();
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

Napi::Object InitAll(Napi::Env env, Napi::Object exports) {
    TRACE("InitAll: module loading");
    TNRPAddon::Init(env, exports);
    TRACE("InitAll: module loaded");
    exports.Set("labelsJson", Napi::Function::New(env, LabelsJson));
    exports.Set("cardColorsJson", Napi::Function::New(env, CardColorsJson));
    return exports;
}

NODE_API_MODULE(protocol_parser, InitAll)
