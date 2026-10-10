#include <jni.h>

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "tnrp/Config.h"
#include "tnrp/BinaryRows.h"
#include "tnrp/PairCrypto.h"
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

// Logical row ids (Engine's rowTypeOf) the Kotlin store consumes: telemetry (its
// packed binary lane is gated by this bit too), status, damage, lap, session,
// timing, participants, all_status, tyre_sets and positions. Everything else
// libtnrp can produce -- race_event, motion, session_history_fastest (a full
// lap/stint history per car in V6), strategy -- would be marshalled through JNI
// and JSON-parsed on the source thread only to be discarded. Control rows
// (protocol_*, recording_error, ...) carry no row id and are never masked, and
// recording ignores the mask, so TNRD files still contain every row.
constexpr uint32_t kAndroidRowMask =
    (1u << 1) | (1u << 2) | (1u << 3) | (1u << 4) | (1u << 5) |
    (1u << 7) | (1u << 8) | (1u << 9) | (1u << 10) | (1u << 13);

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
    config.hotRowsAsJson = false;
    config.binaryPlayback = false;

    gSink = std::make_unique<AndroidSink>(env, receiver);
    gEngine = std::make_unique<tnrp::Engine>(config, gSink.get());
    if (!gEngine->startUdp()) {
        const std::string error = gEngine->udpLastError();
        stopLocked();
        return env->NewStringUTF(error.empty() ? "Unable to bind UDP listener" : error.c_str());
    }
    gEngine->setDataRequirements(kAndroidRowMask, 0, 0.0f);
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


// ── Paired desktop channel ───────────────────────────────────────────────
// The handshake and frame encryption are libtnrp's PairCrypto, the same code
// the desktop runs. Kotlin serialises calls per channel.

namespace {

struct PairChannelHandle {
    tnrp::pair::ClientHandshake handshake;
};

PairChannelHandle* channelOf(jlong handle) {
    return reinterpret_cast<PairChannelHandle*>(static_cast<intptr_t>(handle));
}

std::string utf8(JNIEnv* env, jstring value) {
    if (!value) return {};
    const char* chars = env->GetStringUTFChars(value, nullptr);
    if (!chars) return {};
    std::string out(chars);
    env->ReleaseStringUTFChars(value, chars);
    return out;
}

jstring javaString(JNIEnv* env, const std::string& value) {
    return env->NewStringUTF(value.c_str());
}

jbyteArray javaBytes(JNIEnv* env, const std::vector<uint8_t>& value) {
    jbyteArray out = env->NewByteArray(static_cast<jsize>(value.size()));
    if (out && !value.empty())
        env->SetByteArrayRegion(out, 0, static_cast<jsize>(value.size()),
                                reinterpret_cast<const jbyte*>(value.data()));
    return out;
}

} // namespace

extern "C" JNIEXPORT jlong JNICALL
Java_com_tracknrace_android_PairChannel_nativeNew(JNIEnv*, jobject) {
    return static_cast<jlong>(reinterpret_cast<intptr_t>(new PairChannelHandle()));
}

extern "C" JNIEXPORT void JNICALL
Java_com_tracknrace_android_PairChannel_nativeFree(JNIEnv*, jobject, jlong handle) {
    delete channelOf(handle);
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_tracknrace_android_PairChannel_nativeBegin(
    JNIEnv* env, jobject, jlong handle, jint mode, jstring serverId,
    jstring identityKey, jstring code) {
    auto* channel = channelOf(handle);
    if (!channel) return javaString(env, "Pairing channel is closed");
    const auto chosen = mode == 1 ? tnrp::pair::Mode::Qr
        : mode == 2 ? tnrp::pair::Mode::Code : tnrp::pair::Mode::Resume;
    std::string error;
    if (channel->handshake.begin(chosen, utf8(env, serverId), utf8(env, identityKey),
                                 utf8(env, code), &error)) return nullptr;
    return javaString(env, error.empty() ? "Could not start the connection" : error);
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_tracknrace_android_PairChannel_nativeHello(JNIEnv* env, jobject, jlong handle) {
    auto* channel = channelOf(handle);
    return javaString(env, channel ? channel->handshake.helloJson() : std::string{});
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_tracknrace_android_PairChannel_nativeAccept(
    JNIEnv* env, jobject, jlong handle, jstring serverHello) {
    auto* channel = channelOf(handle);
    if (!channel) return javaString(env, "invalid_server_hello");
    std::string error;
    if (channel->handshake.accept(utf8(env, serverHello), &error)) return nullptr;
    return javaString(env, error.empty() ? "invalid_server_hello" : error);
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_tracknrace_android_PairChannel_nativeIdentityKey(JNIEnv* env, jobject, jlong handle) {
    auto* channel = channelOf(handle);
    return javaString(env, channel ? channel->handshake.identityKey() : std::string{});
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_tracknrace_android_PairChannel_nativeServerId(JNIEnv* env, jobject, jlong handle) {
    auto* channel = channelOf(handle);
    return javaString(env, channel ? channel->handshake.serverId() : std::string{});
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_tracknrace_android_PairChannel_nativeConfirmation(JNIEnv* env, jobject, jlong handle) {
    auto* channel = channelOf(handle);
    return javaString(env, channel ? channel->handshake.confirmation() : std::string{});
}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_tracknrace_android_PairChannel_nativeSeal(
    JNIEnv* env, jobject, jlong handle, jbyteArray text) {
    auto* channel = channelOf(handle);
    if (!channel || !text || !channel->handshake.sender().ready()) return nullptr;
    const jsize length = env->GetArrayLength(text);
    std::vector<uint8_t> plain(static_cast<size_t>(length));
    if (length > 0)
        env->GetByteArrayRegion(text, 0, length, reinterpret_cast<jbyte*>(plain.data()));
    return javaBytes(env, channel->handshake.sender().seal(
        tnrp::pair::kFrameText, plain.data(), plain.size()));
}

// Returns the payload and writes its kind (1 text, 2 binary) to kindOut[0];
// null when the frame fails authentication.
extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_tracknrace_android_PairChannel_nativeOpen(
    JNIEnv* env, jobject, jlong handle, jbyteArray frame, jintArray kindOut) {
    auto* channel = channelOf(handle);
    if (!channel || !frame) return nullptr;
    const jsize length = env->GetArrayLength(frame);
    std::vector<uint8_t> sealed(static_cast<size_t>(length));
    if (length > 0)
        env->GetByteArrayRegion(frame, 0, length, reinterpret_cast<jbyte*>(sealed.data()));
    uint8_t kind = 0;
    std::vector<uint8_t> plain;
    if (!channel->handshake.receiver().open(sealed.data(), sealed.size(), kind, plain))
        return nullptr;
    const jint kindValue = kind;
    if (kindOut && env->GetArrayLength(kindOut) > 0)
        env->SetIntArrayRegion(kindOut, 0, 1, &kindValue);
    return javaBytes(env, plain);
}
