#include "tnrp/TnrdWriter.h"
#include "tnrp/TimeUtils.h"
#include "tnrp/control_rows.h"

#include <algorithm>
#include <chrono>
#include <limits>

#include "protocols/protocol.h"

#ifdef _WIN32
#  include <windows.h>
#endif

namespace tnrp {

static constexpr int PID_SESSION = 1;

// Pulls just session_time out of a stored row for flashback truncation. A row
// without the key keeps the default 0.0f, so it is always kept (matching the
// previous "no session_time => keep" behaviour for non-negative cutoffs).
// Must have external linkage (not in an anonymous namespace): glaze's
// compile-time reflection takes the type's mangled name, which MSVC refuses
// to do for internal-linkage types (error C7631). GCC/Clang allow it.
struct SessionTimeOnly { float session_time{}; };
namespace {
constexpr glz::opts kPartialReadW{ .null_terminated = false, .error_on_unknown_keys = false };
}

static std::string extractType(const std::string& json) {
    static const char KEY[] = "\"type\":\"";
    static constexpr int KLEN = sizeof(KEY) - 1;
    auto pos = json.find(KEY);
    if (pos == std::string::npos) return {};
    pos += KLEN;
    auto end = json.find('"', pos);
    if (end == std::string::npos) return {};
    return json.substr(pos, end - pos);
}

const std::unordered_set<std::string>& TnrdWriter::dedupeTypes() {
    static const std::unordered_set<std::string> kTypes = {
        "session", "tyre_sets", "participants", "all_status", "status", "timing", "damage"
    };
    return kTypes;
}

TnrdWriter::TnrdWriter() {
    diskThread_ = std::thread(&TnrdWriter::writerLoop, this);
}

TnrdWriter::~TnrdWriter() {
    {
        std::unique_lock<std::mutex> lk(mu_);
        WriterEvent ev;
        ev.type = EventType::Close;
        queue_.push(std::move(ev));
        stop_.store(true);
    }
    cv_.notify_all();
    if (diskThread_.joinable()) diskThread_.join();
}

gzFile TnrdWriter::gzOpenPath(const std::string& utf8Path, const char* mode) {
#ifdef _WIN32
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8Path.c_str(), -1, nullptr, 0);
    if (wlen <= 0) return nullptr;
    std::wstring wpath(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8Path.c_str(), -1, wpath.data(), wlen);
    return gzopen_w(wpath.c_str(), mode);
#else
    return gzopen(utf8Path.c_str(), mode);
#endif
}

void TnrdWriter::setLogging(bool enabled, const std::string& outputDir) {
    recording_.store(enabled, std::memory_order_relaxed);
    std::unique_lock<std::mutex> lk(mu_);
    WriterEvent ev;
    ev.type = EventType::SetLogging;
    ev.enabled = enabled;
    ev.outputDir = outputDir;
    queue_.push(std::move(ev));
    cv_.notify_one();
}

void TnrdWriter::notePacket(uint16_t format, uint8_t packetId, float sessionTime,
                            const uint8_t* data, int length) {
    std::unique_lock<std::mutex> lk(mu_);
    if (!data || length <= 0) return;
    WriterEvent ev;
    ev.type = EventType::NotePacket;
    ev.format = format;
    ev.packetId = packetId;
    ev.sessionTime = sessionTime;
    // Only PID_SESSION's bytes are read on the disk thread (track/session detection);
    // copying every other packet's ~1.3 KB into the queue is pure waste.
    if (packetId == PID_SESSION)
        ev.packetData.assign(data, data + length);
    queue_.push(std::move(ev));
    cv_.notify_one();
}

void TnrdWriter::record(const std::string& json, float sessionTime) {
    std::unique_lock<std::mutex> lk(mu_);
    WriterEvent ev;
    ev.type = EventType::Record;
    ev.json = json;
    ev.sessionTime = sessionTime;
    queue_.push(std::move(ev));
    cv_.notify_one();
}

void TnrdWriter::writerLoop() {
    while (true) {
        WriterEvent ev;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [this] { return !queue_.empty(); });
            ev = std::move(queue_.front());
            queue_.pop();
        }

        if (ev.type == EventType::SetLogging) {
            wantRecord_ = ev.enabled;
            outputDirectory_ = ev.outputDir;
            if (!ev.enabled) closeActiveStream();
        } else if (ev.type == EventType::NotePacket) {
            if (activeGzip_ && lastSessionTime_ >= 0.0f && ev.sessionTime < lastSessionTime_ - 0.2f)
                truncateTimeline(ev.sessionTime);
            else if (ev.sessionTime > lastSessionTime_)
                lastSessionTime_ = ev.sessionTime;

            if (ev.packetId == PID_SESSION && ev.packetData.size() >= 708) {
                int8_t  trackId     = ReadInt8(ev.packetData.data(), 36);
                uint8_t sessionType = ev.packetData[35];
                if (wantRecord_ && (trackId != currentTrackId_ ||
                                    sessionType != currentSessionType_ || !activeGzip_))
                    startNewStream(trackId, sessionType, ev.format);
            }
        } else if (ev.type == EventType::Record) {
            if (!activeGzip_) continue;
            std::string type = extractType(ev.json);
            if (isDuplicate(type, ev.json)) continue;
            std::string line = ev.json + "\n";
            float entryTime  = (ev.sessionTime >= 0.0f) ? ev.sessionTime : lastSessionTime_;
            rollingBuffer_.push_back({line, entryTime});
            if (type == "race_event" && ev.json.find("\"code\":\"SEND\"") != std::string::npos) {
                closeActiveStream(); continue;
            }
            flushOldBufferEntries();
        } else if (ev.type == EventType::Close) {
            closeActiveStream();
            if (stop_.load() && queue_.empty()) break;
        }
    }
}

void TnrdWriter::closeActiveStream() {
    if (activeGzip_) {
        flushBufferToDisk(rollingBuffer_);
        gzclose(activeGzip_);
        activeGzip_ = nullptr;
    }
    rollingBuffer_.clear();
    currentTrackId_     = -1;
    currentSessionType_ = -1;
    activeGzipPath_.clear();
    lastSessionTime_    = -1.0f;
    dedupeCache_.clear();
}

void TnrdWriter::startNewStream(int trackId, int sessionType, int format) {
    closeActiveStream();
    if (!wantRecord_ || outputDirectory_.empty()) return;

    std::string proto = (format == 2024) ? "f1_24" : "f1_25";

    auto itTrack = TRACK_NAMES.find(trackId);
    std::string tName = (itTrack != TRACK_NAMES.end())
        ? sanitizeName(itTrack->second) : "track_" + std::to_string(trackId);

    auto itSess = SESSION_NAMES.find(sessionType);
    std::string sName = (itSess != SESSION_NAMES.end())
        ? sanitizeName(itSess->second) : "session_" + std::to_string(sessionType);

    std::string filename = proto + "_" + std::to_string(trackId) + "_"
                         + tName + "_" + sName + "_" + filenameTimestamp() + ".tnrd";

    activeGzipPath_ = outputDirectory_ + "/" + filename;
    activeGzip_     = gzOpenPath(activeGzipPath_, "wb");

    if (activeGzip_) {
        HeaderRow hdr;
        hdr.magic        = "TNRD_V1";
        hdr.protocol     = format;
        hdr.track_id     = trackId;
        hdr.track_name   = (itTrack != TRACK_NAMES.end()) ? itTrack->second : "Unknown";
        hdr.session_type = sessionType;
        hdr.session_name = (itSess != SESSION_NAMES.end()) ? itSess->second : "Unknown";
        hdr.start_time   = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        std::string hl = writeJson(hdr) + "\n";
        gzwrite(activeGzip_, hl.c_str(), (unsigned int)hl.size());

        currentTrackId_     = trackId;
        currentSessionType_ = sessionType;
        lastSessionTime_    = -1.0f;
    }
}

void TnrdWriter::flushBufferToDisk(const std::vector<BufferEntry>& entries) {
    if (!activeGzip_ || entries.empty()) return;
    for (const auto& e : entries)
        gzwrite(activeGzip_, e.line.c_str(), (unsigned int)e.line.size());
}

void TnrdWriter::flushOldBufferEntries() {
    if (lastSessionTime_ < 0.0f || rollingBuffer_.empty()) return;
    float cutoff = lastSessionTime_ - BUFFER_WINDOW_S;
    size_t flush = 0;
    while (flush < rollingBuffer_.size() && rollingBuffer_[flush].sessionTime < cutoff)
        flush++;
    if (flush > 0) {
        flushBufferToDisk({rollingBuffer_.begin(), rollingBuffer_.begin() + (ptrdiff_t)flush});
        rollingBuffer_.erase(rollingBuffer_.begin(), rollingBuffer_.begin() + (ptrdiff_t)flush);
    }
}

bool TnrdWriter::isDuplicate(const std::string& type, const std::string& json) {
    if (!dedupeTypes().count(type)) return false;
    // Parse only for dedup types (≤2 Hz) to strip volatile fields before hashing.
    glz::generic doc;
    if (glz::read_json(doc, json)) return false;
    if (doc.is_object()) {
        doc.get_object().erase("ts");
        doc.get_object().erase("session_time");
    }
    std::string hash;
    if (glz::write_json(doc, hash)) return false;
    auto it = dedupeCache_.find(type);
    if (it != dedupeCache_.end() && it->second == hash) return true;
    dedupeCache_[type] = std::move(hash);
    return false;
}

void TnrdWriter::truncateTimeline(float newSessionTime) {
    float bufStart = rollingBuffer_.empty()
        ? std::numeric_limits<float>::infinity() : rollingBuffer_[0].sessionTime;

    if (newSessionTime >= bufStart) {
        rollingBuffer_.erase(
            std::remove_if(rollingBuffer_.begin(), rollingBuffer_.end(),
                [newSessionTime](const BufferEntry& e) { return e.sessionTime > newSessionTime; }),
            rollingBuffer_.end());
    } else {
        rollingBuffer_.clear();
        if (!activeGzipPath_.empty() && activeGzip_) {
            gzclose(activeGzip_); activeGzip_ = nullptr;
            std::vector<std::string> kept;
            gzFile in = gzOpenPath(activeGzipPath_, "rb");
            if (in) {
                char buf[16384];
                while (gzgets(in, buf, sizeof(buf)) != nullptr) {
                    std::string line(buf);
                    SessionTimeOnly st;
                    (void)glz::read<kPartialReadW>(st, line);
                    if (st.session_time <= newSessionTime)
                        kept.push_back(line);
                }
                gzclose(in);
            }
            gzFile out = gzOpenPath(activeGzipPath_, "wb");
            if (out) {
                for (const auto& l : kept) gzwrite(out, l.c_str(), (unsigned int)l.size());
                gzclose(out);
            }
            activeGzip_ = gzOpenPath(activeGzipPath_, "ab");
        }
    }
    dedupeCache_.clear();
    lastSessionTime_ = newSessionTime;
}

} // namespace tnrp
