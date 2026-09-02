#include <jni.h>

#include <memory>
#include <mutex>
#include <string>

#include "tnrp/Config.h"
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
};

std::unique_ptr<AndroidSink> gSink;
std::unique_ptr<tnrp::Engine> gEngine;

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
    config.hotRowsAsJson = true;
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
