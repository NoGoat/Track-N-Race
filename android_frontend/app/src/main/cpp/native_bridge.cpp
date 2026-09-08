#include <jni.h>

#include <memory>
#include <mutex>
#include <string>

#include "tnrp/Config.h"
#include "tnrp/BinaryRows.h"
#include "tnrp/PairDiscovery.h"
#include "tnrp/Engine.h"
#include "tnrp/Sink.h"

namespace {

JavaVM* gVm = nullptr;
std::mutex gMutex;

class AndroidSink final : public tnrp::Sink {
public:
    AndroidSink(JNIEnv* env, jobject receiver)
        : receiver_(env->NewGlobalRef(receiver)) {
        jclass cls = env->GetObjectClass(receiver);
        onRow_ = env->GetMethodID(cls, "onNativeRow", "(Ljava/lang/String;)V");
        onBinary_ = env->GetMethodID(cls, "onNativeBinary", "([B)V");
        env->DeleteLocalRef(cls);
    }

    ~AndroidSink() override {
        bool attached = false;
        JNIEnv* env = environment(attached);
        if (env && receiver_) env->DeleteGlobalRef(receiver_);
        if (attached) gVm->DetachCurrentThread();
    }

    void onRow(const std::string& json) override {
        bool attached = false;
        JNIEnv* env = environment(attached);
        if (!env || !receiver_ || !onRow_) return;

        jstring value = env->NewStringUTF(json.c_str());
        if (value) {
            env->CallVoidMethod(receiver_, onRow_, value);
            env->DeleteLocalRef(value);
        }
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (attached) gVm->DetachCurrentThread();
    }

    void onBinary(const uint8_t* data, size_t len) override {
        // The dashboard consumes telemetry plus all-car positions. The filter
        // takes logical row ids (1 and 13), not packed wire tags (1 and 3).
        thread_local std::vector<uint8_t> dashboardRows;
        dashboardRows.clear();
        dashboardRows.reserve(len);
        constexpr uint32_t mask = (1u << 1) | (1u << 13);
        if (!tnrp::bin::appendFilteredBatch(dashboardRows, data, len, mask)
            || dashboardRows.empty()) return;

        bool attached = false;
        JNIEnv* env = environment(attached);
        if (!env || !receiver_ || !onBinary_) return;

        jbyteArray value = env->NewByteArray(static_cast<jsize>(dashboardRows.size()));
        if (value) {
            env->SetByteArrayRegion(value, 0, static_cast<jsize>(dashboardRows.size()),
                reinterpret_cast<const jbyte*>(dashboardRows.data()));
            env->CallVoidMethod(receiver_, onBinary_, value);
            env->DeleteLocalRef(value);
        }
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (attached) gVm->DetachCurrentThread();
    }

private:
    JNIEnv* environment(bool& attached) const {
        JNIEnv* env = nullptr;
        const jint state = gVm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
        if (state == JNI_OK) return env;
        if (state != JNI_EDETACHED ||
            gVm->AttachCurrentThread(&env, nullptr) != JNI_OK) return nullptr;
        attached = true;
        return env;
    }

    jobject receiver_{};
    jmethodID onRow_{};
    jmethodID onBinary_{};
};

std::unique_ptr<AndroidSink> gSink;
std::unique_ptr<tnrp::Engine> gEngine;
std::unique_ptr<tnrp::PairDiscoveryBrowser> gDiscovery;
std::mutex gDiscoveryMutex;
jobject gDiscoveryReceiver = nullptr;
jmethodID gDiscoveryCallback = nullptr;

void stopLocked() {
    gEngine.reset();
    gSink.reset();
}

} // namespace

extern "C" jint JNI_OnLoad(JavaVM* vm, void*) {
    gVm = vm;
    return JNI_VERSION_1_6;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_tracknrace_android_NativeTelemetry_nativeStart(
    JNIEnv* env, jobject receiver, jint port) {
    std::lock_guard lock(gMutex);
    stopLocked();

    tnrp::Config config;
    config.port = static_cast<uint16_t>(port);
    config.bindAddress = "0.0.0.0";
    config.protocol = tnrp::Override::Auto;
    config.hotRowsAsJson = false;
    config.binaryPlayback = false;

    gSink = std::make_unique<AndroidSink>(env, receiver);
    gEngine = std::make_unique<tnrp::Engine>(config, gSink.get());
    if (!gEngine->startUdp()) {
        const std::string error = gEngine->udpLastError();
        stopLocked();
        return env->NewStringUTF(error.empty() ? "Unable to bind UDP listener" : error.c_str());
    }
    return nullptr;
}

extern "C" JNIEXPORT void JNICALL
Java_com_tracknrace_android_NativeTelemetry_nativeStop(JNIEnv*, jobject) {
    std::lock_guard lock(gMutex);
    stopLocked();
}

extern "C" JNIEXPORT void JNICALL
Java_com_tracknrace_android_NativeTelemetry_nativeSetRecording(
    JNIEnv* env, jobject, jboolean enabled, jstring outputDirectory) {
    std::lock_guard lock(gMutex);
    if (!gEngine) return;

    std::string directory;
    if (outputDirectory) {
        const char* utf = env->GetStringUTFChars(outputDirectory, nullptr);
        if (utf) {
            directory = utf;
            env->ReleaseStringUTFChars(outputDirectory, utf);
        }
    }
    gEngine->setLogging(enabled == JNI_TRUE, directory);
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_tracknrace_android_NativePairDiscovery_nativeStart(
    JNIEnv* env, jobject receiver) {
    std::unique_ptr<tnrp::PairDiscoveryBrowser> previous;
    {
        std::lock_guard lock(gDiscoveryMutex);
        previous = std::move(gDiscovery);
    }
    if (previous) previous->stop();
    std::lock_guard lock(gDiscoveryMutex);
    if (gDiscoveryReceiver) env->DeleteGlobalRef(gDiscoveryReceiver);
    gDiscoveryReceiver = env->NewGlobalRef(receiver);
    jclass cls = env->GetObjectClass(receiver);
    gDiscoveryCallback = env->GetMethodID(cls, "onNativeService",
        "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;IZ)V");
    env->DeleteLocalRef(cls);
    gDiscovery = std::make_unique<tnrp::PairDiscoveryBrowser>();
    std::string error;
    const bool started = gDiscovery->start([](const tnrp::DiscoveredPairService& service) {
        bool attached = false;
        JNIEnv* callbackEnv = nullptr;
        const jint state = gVm->GetEnv(reinterpret_cast<void**>(&callbackEnv), JNI_VERSION_1_6);
        if (state == JNI_EDETACHED && gVm->AttachCurrentThread(&callbackEnv, nullptr) == JNI_OK)
            attached = true;
        if (!callbackEnv) return;
        jobject receiverRef = nullptr;
        jmethodID callback = nullptr;
        {
            std::lock_guard callbackLock(gDiscoveryMutex);
            // Keep the receiver alive after releasing the mutex. Pairing-screen
            // disposal and scanner lifecycle can call nativeStop() here and
            // delete the global reference while this callback is in flight.
            if (gDiscoveryReceiver)
                receiverRef = callbackEnv->NewLocalRef(gDiscoveryReceiver);
            callback = gDiscoveryCallback;
        }
        if (receiverRef && callback) {
            jstring id = callbackEnv->NewStringUTF(service.serverId.c_str());
            jstring name = callbackEnv->NewStringUTF(service.name.c_str());
            jstring address = callbackEnv->NewStringUTF(service.address.c_str());
            callbackEnv->CallVoidMethod(receiverRef, callback, id, name, address,
                static_cast<jint>(service.port), service.pairing ? JNI_TRUE : JNI_FALSE);
            callbackEnv->DeleteLocalRef(id);
            callbackEnv->DeleteLocalRef(name);
            callbackEnv->DeleteLocalRef(address);
            if (callbackEnv->ExceptionCheck()) callbackEnv->ExceptionClear();
        }
        if (receiverRef) callbackEnv->DeleteLocalRef(receiverRef);
        if (attached) gVm->DetachCurrentThread();
    }, &error);
    if (!started) {
        gDiscovery.reset();
        env->DeleteGlobalRef(gDiscoveryReceiver);
        gDiscoveryReceiver = nullptr;
        gDiscoveryCallback = nullptr;
        return env->NewStringUTF(error.c_str());
    }
    return nullptr;
}

extern "C" JNIEXPORT void JNICALL
Java_com_tracknrace_android_NativePairDiscovery_nativeStop(JNIEnv* env, jobject) {
    std::unique_ptr<tnrp::PairDiscoveryBrowser> browser;
    {
        std::lock_guard lock(gDiscoveryMutex);
        browser = std::move(gDiscovery);
    }
    if (browser) browser->stop();
    std::lock_guard lock(gDiscoveryMutex);
    if (gDiscoveryReceiver) env->DeleteGlobalRef(gDiscoveryReceiver);
    gDiscoveryReceiver = nullptr;
    gDiscoveryCallback = nullptr;
}
