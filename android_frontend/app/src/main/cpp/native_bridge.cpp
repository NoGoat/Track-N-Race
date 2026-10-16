#include <jni.h>

#include <cctype>
#include <cstdio>
#include <memory>
#include <optional>
#include <mutex>
#include <string_view>
#include <string>
#include <vector>

#include "tnrp/Config.h"
#include "tnrp/BinaryRows.h"
#include "tnrp/PairCrypto.h"
#include "tnrp/Engine.h"
#include "tnrp/PairServer.h"
#include "tnrp/Parser.h"
#include "tnrp/Sink.h"
#include "tnrp/TnrdReader.h"

namespace {

JavaVM* gVm = nullptr;
std::mutex gMutex;

class AndroidSink final : public tnrp::Sink {
public:
    AndroidSink(JNIEnv* env, jobject receiver, bool playback = false)
        : receiver_(env->NewGlobalRef(receiver)), playback_(playback) {
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
        // A recording's lap catalogue carries every block's status history;
        // the phone needs only each lap's times and compound, as a paired
        // phone receives it.
        if (std::string_view(json).starts_with("{\"type\":\"playback_lap_blocks\"")) {
            deliver(tnrp::pairPhoneLapBlocks(json));
            return;
        }
        deliver(json);
        // A JSON-only engine does not restate the protocol when a recording
        // loads. Label the recording with its own format's catalogue, as the
        // desktop does, so track names and aero labels follow the file.
        if (playback_ && std::string_view(json).starts_with("{\"type\":\"playback_loaded\",\"ok\":true")) {
            const auto protocol = intAfter(json, "\"protocol\":");
            if (protocol && *protocol >= 2024) {
                std::optional<bool> regulations;
                if (json.find("\"regulations_2026\":true") != std::string::npos) regulations = true;
                else if (json.find("\"regulations_2026\":false") != std::string::npos) regulations = false;
                deliver(tnrp::Parser::statusRowForFormat(
                    static_cast<uint16_t>(*protocol), intAfter(json, "\"formula\":"), regulations));
            }
        }
    }

    void deliver(const std::string& json) {
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
    static std::optional<int> intAfter(const std::string& json, std::string_view key) {
        const size_t at = json.find(key);
        if (at == std::string::npos) return std::nullopt;
        size_t index = at + key.size();
        const bool negative = index < json.size() && json[index] == '-';
        if (negative) ++index;
        if (index >= json.size() || !std::isdigit(static_cast<unsigned char>(json[index]))) return std::nullopt;
        int value = 0;
        while (index < json.size() && std::isdigit(static_cast<unsigned char>(json[index])))
            value = value * 10 + (json[index++] - '0');
        return negative ? -value : value;
    }

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
    bool playback_{};
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

// One engine at a time: listening for the game, or playing a recording. A
// call takes its own reference, so a slow seek or lap read finishing after
// stop() still has an engine, and the engine always goes before its sink.
struct Session {
    std::unique_ptr<AndroidSink> sink;
    std::unique_ptr<tnrp::Engine> engine;

    ~Session() { engine.reset(); }
};

std::shared_ptr<Session> gSession;

void stopLocked() {
    gSession.reset();
}

std::shared_ptr<Session> session() {
    std::lock_guard lock(gMutex);
    return gSession;
}

tnrp::Config androidConfig() {
    tnrp::Config config;
    config.bindAddress = "0.0.0.0";
    config.protocol = tnrp::Override::Auto;
    config.hotRowsAsJson = false;
    config.binaryPlayback = false;
    return config;
}

std::string utf8(JNIEnv* env, jstring value);

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

    tnrp::Config config = androidConfig();
    config.port = static_cast<uint16_t>(port);

    auto next = std::make_shared<Session>();
    next->sink = std::make_unique<AndroidSink>(env, receiver);
    next->engine = std::make_unique<tnrp::Engine>(config, next->sink.get());
    if (!next->engine->startUdp()) {
        const std::string error = next->engine->udpLastError();
        return env->NewStringUTF(error.empty() ? "Unable to bind UDP listener" : error.c_str());
    }
    next->engine->setDataRequirements(kAndroidRowMask, 0, 0.0f);
    gSession = std::move(next);
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
    if (!gSession) return;

    std::string directory;
    if (outputDirectory) {
        const char* utf = env->GetStringUTFChars(outputDirectory, nullptr);
        if (utf) {
            directory = utf;
            env->ReleaseStringUTFChars(outputDirectory, utf);
        }
    }
    gSession->engine->setLogging(enabled == JNI_TRUE, directory);
}


// ── Recording playback ───────────────────────────────────────────────────
// A recording on the phone plays through the same engine the desktop uses,
// with no UDP listener. Its rows reach the store exactly as live rows do.

extern "C" JNIEXPORT jstring JNICALL
Java_com_tracknrace_android_NativeTelemetry_nativeOpenRecording(
    JNIEnv* env, jobject receiver, jstring path) {
    const std::string file = utf8(env, path);
    auto next = std::make_shared<Session>();
    next->sink = std::make_unique<AndroidSink>(env, receiver, true);
    next->engine = std::make_unique<tnrp::Engine>(androidConfig(), next->sink.get());
    {
        std::lock_guard lock(gMutex);
        stopLocked();
        gSession = next;
    }
    std::string error;
    if (next->engine->playerLoad(file, &error)) return nullptr;
    {
        std::lock_guard lock(gMutex);
        if (gSession == next) stopLocked();
    }
    return env->NewStringUTF(error.empty() ? "Could not open the recording" : error.c_str());
}

extern "C" JNIEXPORT void JNICALL
Java_com_tracknrace_android_NativeTelemetry_nativePlay(JNIEnv*, jobject) {
    if (auto current = session()) current->engine->playerPlay();
}

extern "C" JNIEXPORT void JNICALL
Java_com_tracknrace_android_NativeTelemetry_nativePause(JNIEnv*, jobject) {
    if (auto current = session()) current->engine->playerPause();
}

extern "C" JNIEXPORT void JNICALL
Java_com_tracknrace_android_NativeTelemetry_nativeRequestSeek(JNIEnv*, jobject, jlong requestId) {
    if (auto current = session()) current->engine->playerRequestSeek(static_cast<uint64_t>(requestId));
}

extern "C" JNIEXPORT void JNICALL
Java_com_tracknrace_android_NativeTelemetry_nativeSeek(
    JNIEnv*, jobject, jfloat fraction, jlong requestId) {
    if (auto current = session())
        current->engine->playerSeek(fraction, false, static_cast<uint64_t>(requestId));
}

extern "C" JNIEXPORT void JNICALL
Java_com_tracknrace_android_NativeTelemetry_nativeSetSpeed(JNIEnv*, jobject, jfloat speed) {
    if (auto current = session()) current->engine->playerSetSpeed(speed);
}

extern "C" JNIEXPORT void JNICALL
Java_com_tracknrace_android_NativeTelemetry_nativeSetDriver(JNIEnv*, jobject, jint driver) {
    if (auto current = session()) current->engine->playerSetDriver(driver);
}

extern "C" JNIEXPORT void JNICALL
Java_com_tracknrace_android_NativeTelemetry_nativeSetRequirements(
    JNIEnv* env, jobject, jint streamMask, jintArray v6Types) {
    auto current = session();
    if (!current) return;
    std::vector<uint8_t> types;
    if (v6Types) {
        const jsize count = env->GetArrayLength(v6Types);
        std::vector<jint> values(static_cast<size_t>(count));
        if (count > 0) env->GetIntArrayRegion(v6Types, 0, count, values.data());
        for (jint value : values)
            if (value > 0 && value < 256) types.push_back(static_cast<uint8_t>(value));
    }
    current->engine->setPhoneRequirements(static_cast<uint32_t>(streamMask), types);
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_tracknrace_android_NativeTelemetry_nativeLapDelta(
    JNIEnv* env, jobject, jint currentLap, jint comparisonLap, jboolean sectorDelta) {
    auto current = session();
    if (!current) return nullptr;
    const std::string data = current->engine->playerLapDeltaJson(
        currentLap, comparisonLap, sectorDelta == JNI_TRUE);
    return data.empty() ? nullptr : env->NewStringUTF(data.c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_tracknrace_android_NativeTelemetry_nativeLapData(
    JNIEnv* env, jobject, jint lapNum, jobjectArray channels) {
    auto current = session();
    if (!current) return nullptr;
    std::vector<std::string> names;
    if (channels) {
        const jsize count = env->GetArrayLength(channels);
        names.reserve(static_cast<size_t>(count));
        for (jsize index = 0; index < count; ++index) {
            auto* name = static_cast<jstring>(env->GetObjectArrayElement(channels, index));
            names.push_back(utf8(env, name));
            env->DeleteLocalRef(name);
        }
    }
    const std::string data = current->engine->playerPhoneLapDataJson(lapNum, names);
    return data.empty() ? nullptr : env->NewStringUTF(data.c_str());
}

namespace {

std::string jsonString(const std::string& value) {
    std::string out = "\"";
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
        else if (c < 0x20) { char escaped[8]; std::snprintf(escaped, sizeof(escaped), "\\u%04x", c); out += escaped; }
        else out += static_cast<char>(c);
    }
    return out + "\"";
}

} // namespace

// A recording's details for the file list, read from its index without
// loading it: {"protocol","trackName","sessionName","sessionType",
// "startTime" (ms since the epoch),"duration" (s),"drivers"}, or null.
extern "C" JNIEXPORT jstring JNICALL
Java_com_tracknrace_android_NativeTelemetry_nativeSummarize(JNIEnv* env, jclass, jstring path) {
    tnrp::TnrdReader::Summary summary;
    if (!tnrp::TnrdReader::summarize(utf8(env, path), summary)) return nullptr;
    const auto& header = summary.header;
    const std::string json = "{\"protocol\":" + std::to_string(header.protocol) +
        ",\"trackName\":" + jsonString(header.track_name) +
        ",\"sessionName\":" + jsonString(header.session_name) +
        ",\"sessionType\":" + std::to_string(header.session_type) +
        ",\"startTime\":" + std::to_string(header.start_time) +
        ",\"duration\":" + std::to_string(summary.durationSeconds) +
        ",\"drivers\":" + std::to_string(summary.drivers) + "}";
    return env->NewStringUTF(json.c_str());
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
