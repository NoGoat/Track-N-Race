#include "tnrp/TnrdWriter.h"
#include "tnrp/TimeUtils.h"
#include "tnrp/control_rows.h"
#include "TnrdCodec.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
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
        "session", "tyre_sets", "participants", "all_status", "timing", "damage"
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

void TnrdWriter::setLogging(bool enabled, const std::string& outputDir) {
    setLoggingZstd(enabled, outputDir);
}

void TnrdWriter::setLoggingZstd(bool enabled, const std::string& outputDir) {
    setLoggingForFormat(enabled, outputDir, TnrdFormat::ZstdV2);
}

void TnrdWriter::setLoggingGzip(bool enabled, const std::string& outputDir) {
    setLoggingForFormat(enabled, outputDir, TnrdFormat::GzipV1);
}

void TnrdWriter::setLoggingForFormat(bool enabled, const std::string& outputDir,
                                     TnrdFormat format) {
    recording_.store(enabled, std::memory_order_relaxed);
    std::unique_lock<std::mutex> lk(mu_);
    WriterEvent ev;
    ev.type = EventType::SetLogging;
    ev.enabled = enabled;
    ev.outputDir = outputDir;
    ev.tnrdFormat = format;
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
            const bool formatChanged = wantRecord_ && writeFormat_ != ev.tnrdFormat;
            wantRecord_ = ev.enabled;
            outputDirectory_ = ev.outputDir;
            writeFormat_ = ev.tnrdFormat;
            if (!ev.enabled || formatChanged) closeActiveStream();
        } else if (ev.type == EventType::NotePacket) {
            if (activeStream_ && lastSessionTime_ >= 0.0f && ev.sessionTime < lastSessionTime_ - 0.2f)
                truncateTimeline(ev.sessionTime);
            else if (ev.sessionTime > lastSessionTime_)
                lastSessionTime_ = ev.sessionTime;

            if (ev.packetId == PID_SESSION && ev.packetData.size() >= 708) {
                int8_t  trackId     = ReadInt8(ev.packetData.data(), 36);
                uint8_t sessionType = ev.packetData[35];
                if (wantRecord_ && (trackId != currentTrackId_ ||
                                    sessionType != currentSessionType_ || !activeStream_))
                    startNewStream(trackId, sessionType, ev.format);
            }
        } else if (ev.type == EventType::Record) {
            if (!activeStream_) continue;
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
    if (activeStream_) {
        flushBufferToDisk(rollingBuffer_);
        if (!activeStream_->finish())
            std::fprintf(stderr, "[tnrd] writer close failed for '%s': %s\n",
                         activePath_.c_str(), activeStream_->error().c_str());
        activeStream_.reset();
    }
    rollingBuffer_.clear();
    currentTrackId_     = -1;
    currentSessionType_ = -1;
    activePath_.clear();
    lastSessionTime_    = -1.0f;
    rowsSinceFlush_     = 0;
    dedupeCache_.clear();
}

void TnrdWriter::startNewStream(int trackId, int sessionType, int format) {
    closeActiveStream();
    if (!wantRecord_ || outputDirectory_.empty()) return;

    const std::string proto = RecordingFilenamePrefix(format);

    auto itTrack = TRACK_NAMES.find(trackId);
    std::string tName = (itTrack != TRACK_NAMES.end())
        ? sanitizeName(itTrack->second) : "track_" + std::to_string(trackId);

    auto itSess = SESSION_NAMES.find(sessionType);
    std::string sName = (itSess != SESSION_NAMES.end())
        ? sanitizeName(itSess->second) : "session_" + std::to_string(sessionType);

    std::string filename = proto + "_" + std::to_string(trackId) + "_"
                         + tName + "_" + sName + "_" + filenameTimestamp() + ".tnrd";

    activePath_ = outputDirectory_ + "/" + filename;
    std::string openError;
    activeStream_ = detail::openTnrdOutput(activePath_, writeFormat_, false, &openError);

    if (activeStream_) {
        HeaderRow hdr;
        hdr.magic        = writeFormat_ == TnrdFormat::ZstdV2 ? "TNRD_V2" : "TNRD_V1";
        if (writeFormat_ == TnrdFormat::ZstdV2) hdr.compression = "zstd";
        hdr.protocol     = format;
        hdr.track_id     = trackId;
        hdr.track_name   = (itTrack != TRACK_NAMES.end()) ? itTrack->second : "Unknown";
        hdr.session_type = sessionType;
        hdr.session_name = (itSess != SESSION_NAMES.end()) ? itSess->second : "Unknown";
        hdr.start_time   = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        std::string hl = writeJson(hdr) + "\n";
        if (!activeStream_->write(hl)) {
            std::fprintf(stderr, "[tnrd] writer header failed for '%s': %s\n",
                         activePath_.c_str(), activeStream_->error().c_str());
            activeStream_.reset();
            activePath_.clear();
            return;
        }

        currentTrackId_     = trackId;
        currentSessionType_ = sessionType;
        lastSessionTime_    = -1.0f;
        rowsSinceFlush_     = 0;
    } else {
        std::fprintf(stderr, "[tnrd] writer open failed for '%s': %s\n",
                     activePath_.c_str(), openError.c_str());
        activePath_.clear();
    }
}

void TnrdWriter::flushBufferToDisk(const std::vector<BufferEntry>& entries) {
    if (!activeStream_ || entries.empty()) return;
    for (const auto& e : entries) {
        if (!activeStream_->write(e.line)) {
            std::fprintf(stderr, "[tnrd] writer data failed for '%s': %s\n",
                         activePath_.c_str(), activeStream_->error().c_str());
            return;
        }
    }

    // Periodically emit a codec-specific recoverability point. Both zlib's sync
    // flush and Zstandard's stream flush make all complete rows supplied so far
    // immediately decodable without ending the active member/frame.
    rowsSinceFlush_ += (int)entries.size();
    if (rowsSinceFlush_ >= FLUSH_EVERY_ROWS) {
        if (!activeStream_->flushRecoverable())
            std::fprintf(stderr, "[tnrd] writer flush failed for '%s': %s\n",
                         activePath_.c_str(), activeStream_->error().c_str());
        rowsSinceFlush_ = 0;
    }
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
    // Parse only for deduped state types to strip volatile fields before hashing.
    // Player status is intentionally not deduped: its menu-rate sample timestamps
    // are chart history and must survive recording even when values hold steady.
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
        if (!activePath_.empty() && activeStream_) {
            if (!activeStream_->finish())
                std::fprintf(stderr, "[tnrd] writer close before flashback failed: %s\n",
                             activeStream_->error().c_str());
            activeStream_.reset();

            std::vector<std::string> kept;
            const std::string plainPath = activePath_ + ".rewrite.jsonl.tmp";
            const std::string compressedPath = activePath_ + ".rewrite.tnrd.tmp";
            bool partial = false;
            std::string codecError;
            const bool decompressed = detail::decompressTnrd(
                activePath_, plainPath, writeFormat_, &partial, &codecError);
            if (decompressed) {
                std::ifstream in(std::filesystem::u8path(plainPath), std::ios::binary);
                std::string line;
                while (std::getline(in, line)) {
                    line.push_back('\n');
                    SessionTimeOnly st;
                    (void)glz::read<kPartialReadW>(st, line);
                    if (st.session_time <= newSessionTime)
                        kept.push_back(line);
                }
            } else {
                std::fprintf(stderr, "[tnrd] flashback decompress failed for '%s': %s\n",
                             activePath_.c_str(), codecError.c_str());
            }

            bool replaced = false;
            codecError.clear();
            auto rewritten = decompressed && !kept.empty()
                ? detail::openTnrdOutput(compressedPath, writeFormat_, false, &codecError)
                : nullptr;
            if (rewritten) {
                bool writeOk = true;
                for (const auto& line : kept) {
                    if (!rewritten->write(line)) { writeOk = false; break; }
                }
                writeOk = rewritten->finish() && writeOk;
                rewritten.reset();
                if (writeOk) {
                    std::error_code ec;
#ifdef _WIN32
                    const auto src = std::filesystem::u8path(compressedPath).wstring();
                    const auto dst = std::filesystem::u8path(activePath_).wstring();
                    replaced = MoveFileExW(src.c_str(), dst.c_str(),
                                           MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
                    std::filesystem::rename(compressedPath, activePath_, ec);
                    replaced = !ec;
#endif
                }
            }
            if (!replaced)
                std::fprintf(stderr, "[tnrd] flashback rewrite failed for '%s': %s\n",
                             activePath_.c_str(), codecError.c_str());

            std::error_code cleanupError;
            std::filesystem::remove(std::filesystem::u8path(plainPath), cleanupError);
            std::filesystem::remove(std::filesystem::u8path(compressedPath), cleanupError);

            codecError.clear();
            activeStream_ = detail::openTnrdOutput(activePath_, writeFormat_, true, &codecError);
            if (!activeStream_)
                std::fprintf(stderr, "[tnrd] flashback append reopen failed for '%s': %s\n",
                             activePath_.c_str(), codecError.c_str());
        }
    }
    dedupeCache_.clear();
    lastSessionTime_ = newSessionTime;
}

} // namespace tnrp
