#include <napi.h>
#include <tnrp/Engine.h>
#include <iostream>
#include <memory>

using namespace Napi;

// Convert nlohmann::json to Napi::Value directly
Napi::Value jsonToNapi(napi_env env, const nlohmann::json& j) {
    if (j.is_null()) return Env(env).Null();
    if (j.is_boolean()) return Napi::Boolean::New(env, j.get<bool>());
    if (j.is_number_integer()) return Napi::Number::New(env, j.get<int64_t>());
    if (j.is_number_unsigned()) return Napi::Number::New(env, j.get<uint64_t>());
    if (j.is_number_float()) return Napi::Number::New(env, j.get<double>());
    if (j.is_string()) return Napi::String::New(env, j.get<std::string>());
    if (j.is_array()) {
        Napi::Array arr = Napi::Array::New(env, j.size());
        for (size_t i = 0; i < j.size(); ++i) {
            arr.Set(i, jsonToNapi(env, j[i]));
        }
        return arr;
    }
    if (j.is_object()) {
        Napi::Object obj = Napi::Object::New(env);
        for (auto it = j.begin(); it != j.end(); ++it) {
            obj.Set(it.key(), jsonToNapi(env, it.value()));
        }
        return obj;
    }
    return Env(env).Undefined();
}

class TNRPAddon : public Napi::ObjectWrap<TNRPAddon>, public tnrp::Sink {
public:
    static Napi::Object Init(Napi::Env env, Napi::Object exports) {
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
            InstanceMethod("destroy", &TNRPAddon::Destroy)
        });

        Napi::FunctionReference* constructor = new Napi::FunctionReference();
        *constructor = Napi::Persistent(func);
        env.SetInstanceData(constructor);

        exports.Set("Engine", func);
        return exports;
    }

    TNRPAddon(const Napi::CallbackInfo& info) : Napi::ObjectWrap<TNRPAddon>(info) {
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

        Napi::Function cb = info[1].As<Napi::Function>();
        
        // Create ThreadSafeFunction
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

        engine = std::make_unique<tnrp::Engine>(config, this);
    }

    ~TNRPAddon() {
        if (!destroyed_) {
            if (engine) engine->playerClose();
            tsfn.Release();
        }
    }

    void onRow(const nlohmann::json& row) override {
        auto* rowCopy = new nlohmann::json(row);
        
        auto callback = [](Napi::Env env, Napi::Function jsCallback, nlohmann::json* data) {
            if (env != nullptr && jsCallback != nullptr) {
                jsCallback.Call({ jsonToNapi(env, *data) });
            }
            delete data;
        };

        tsfn.BlockingCall(rowCopy, callback);
    }

private:
    std::unique_ptr<tnrp::Engine> engine;
    Napi::ThreadSafeFunction tsfn;
    bool destroyed_ = false;

    Napi::Value StartUdp(const Napi::CallbackInfo& info) {
        return Napi::Boolean::New(info.Env(), engine->startUdp());
    }

    Napi::Value SetOverride(const Napi::CallbackInfo& info) {
        if (info.Length() >= 1 && info[0].IsString()) {
            std::string ovr = info[0].As<Napi::String>().Utf8Value();
            if (ovr == "auto") engine->setOverride(tnrp::Override::Auto);
            else if (ovr == "f1_24") engine->setOverride(tnrp::Override::F1_24);
            else if (ovr == "f1_25") engine->setOverride(tnrp::Override::F1_25);
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
        return info.Env().Undefined();
    }

    Napi::Value PlayerClose(const Napi::CallbackInfo& info) {
        if (engine) engine->playerClose();
        return info.Env().Undefined();
    }
};

Napi::Object InitAll(Napi::Env env, Napi::Object exports) {
    return TNRPAddon::Init(env, exports);
}

NODE_API_MODULE(protocol_parser, InitAll)
