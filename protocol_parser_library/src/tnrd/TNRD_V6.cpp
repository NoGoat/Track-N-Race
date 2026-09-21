#include "TNRD_V6.h"
#include "TnrdCodec.h"
#include "tnrp/rows.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <list>
#include <map>
#include <mutex>
#include <set>
#include <tuple>
#include <unordered_map>

#include <glaze/glaze.hpp>
#include <zlib.h>
#include <zstd.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace tnrp::detail {

// External linkage is required by Glaze's compile-time aggregate reflection
// on MSVC (internal-linkage aggregate types trigger C7631).
struct V6Metadata {
    HeaderRow session;
    std::vector<V6DriverHeader> drivers;
    std::vector<V6LapSummary> laps;
    struct StoredShared {
        V6Phase phase{V6Phase::Race};
        float sessionTime{};
        uint64_t offset{};
        uint64_t compressedSize{};
        uint64_t uncompressedSize{};
        uint32_t checksum{};
    };
    std::vector<StoredShared> shared;
};

namespace {

constexpr size_t HEADER_SIZE = 128;
constexpr size_t FOOTER_SIZE = 48;
constexpr size_t CHUNK_PREFIX_SIZE = 32;
constexpr size_t CHUNK_ENTRY_SIZE = 56;
constexpr uint32_t CHUNK_MAGIC = 0x364b4843u;  // CHK6
constexpr uint32_t SHARED_MAGIC = 0x36524853u; // SHR6
constexpr uint32_t SESSION_MAGIC = 0x36534553u; // SES6
constexpr uint32_t FOOTER_MAGIC = 0x36444e45u; // END6
constexpr uint32_t MAX_CHUNKS = 5'000'000;
constexpr uint64_t MAX_CHUNK_PLAIN = 512ull * 1024ull * 1024ull;
constexpr uint64_t MAX_METADATA_BYTES = 128ull * 1024ull * 1024ull;
constexpr size_t DEFAULT_CACHE_BYTES = 64ull * 1024ull * 1024ull;
constexpr float WRITE_DELAY = 30.0f;
constexpr int DEFAULT_COMPRESSION_LEVEL = 3;
constexpr glz::opts kPartialRead{.null_terminated = false, .error_on_unknown_keys = false};
const std::array<uint8_t, 8> MAGIC{{'T','N','R','D','_','V','6','\0'}};

void fail(std::string* out, std::string value) { if (out) *out = std::move(value); }
bool writeAll(std::FILE* f, const void* p, size_t n) {
    return n == 0 || (f && std::fwrite(p, 1, n, f) == n);
}
bool seekFile(std::FILE* f, uint64_t offset) {
#ifdef _WIN32
    return f && offset <= static_cast<uint64_t>(std::numeric_limits<__int64>::max()) &&
        _fseeki64(f, static_cast<__int64>(offset), SEEK_SET) == 0;
#else
    return f && offset <= static_cast<uint64_t>(std::numeric_limits<off_t>::max()) &&
        fseeko(f, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
}
bool seekEnd(std::FILE* f) {
#ifdef _WIN32
    return f && _fseeki64(f, 0, SEEK_END) == 0;
#else
    return f && fseeko(f, 0, SEEK_END) == 0;
#endif
}
uint64_t tellFile(std::FILE* f) {
#ifdef _WIN32
    const auto value = f ? _ftelli64(f) : -1;
#else
    const auto value = f ? ftello(f) : -1;
#endif
    return value < 0 ? UINT64_MAX : static_cast<uint64_t>(value);
}
bool readAt(std::FILE* f, uint64_t offset, void* p, size_t n) {
    return seekFile(f, offset) && (n == 0 || std::fread(p, 1, n, f) == n);
}
bool rangeOk(uint64_t size, uint64_t offset, uint64_t bytes) {
    return offset <= size && bytes <= size - offset;
}
void put16(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value)); out.push_back(static_cast<uint8_t>(value >> 8));
}
void put32(std::vector<uint8_t>& out, uint32_t value) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(value >> (i * 8)));
}
void put64(std::vector<uint8_t>& out, uint64_t value) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>(value >> (i * 8)));
}
void putFloat(std::vector<uint8_t>& out, float value) {
    uint32_t bits{}; std::memcpy(&bits, &value, sizeof(bits)); put32(out, bits);
}
uint16_t get16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | uint16_t(p[1]) << 8); }
uint32_t get32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
uint64_t get64(const uint8_t* p) {
    uint64_t value{}; for (int i = 0; i < 8; ++i) value |= uint64_t(p[i]) << (i * 8); return value;
}
float getFloat(const uint8_t* p) {
    const uint32_t bits = get32(p); float value{}; std::memcpy(&value, &bits, sizeof(value)); return value;
}

template <class T> std::string jsonOf(const T& value) {
    std::string out;
    if (glz::write_json(value, out)) return {};
    return out;
}
float scanTime(std::string_view json) {
    const auto key = json.find("\"session_time\":");
    return key == json.npos ? -1.0f : std::strtof(json.data() + key + 15, nullptr);
}
std::string rowType(std::string_view json) {
    auto at = json.find("\"type\":\"");
    if (at == json.npos) return {};
    at += 8; const auto end = json.find('"', at);
    return end == json.npos ? std::string{} : std::string(json.substr(at, end - at));
}
std::string number(double value) {
    char buffer[64]; std::snprintf(buffer, sizeof(buffer), "%.9g", value); return buffer;
}
std::string integer(int64_t value) { return std::to_string(value); }
std::string boolean(bool value) { return value ? "true" : "false"; }
std::string sample(float time, std::initializer_list<std::pair<std::string_view, std::string>> fields) {
    std::string out = "{\"session_time\":" + number(time);
    for (const auto& [key, value] : fields) {
        out += ",\""; out.append(key); out += "\":"; out += value;
    }
    out.push_back('}');
    return out;
}
std::string sample(float time, const std::vector<std::pair<std::string, std::string>>& fields) {
    std::string out = "{\"session_time\":" + number(time);
    for (const auto& [key, value] : fields) out += ",\"" + key + "\":" + value;
    out.push_back('}'); return out;
}
std::string retime(std::string_view json, float time) {
    const auto at = json.find("\"session_time\":");
    if (at == json.npos) return std::string(json);
    const auto start = at + 15;
    auto end = start;
    while (end < json.size() && json[end] != ',' && json[end] != '}') ++end;
    std::string out(json); out.replace(start, end - start, number(time)); return out;
}
bool stateType(V6DataType type) {
    switch (type) {
        case V6DataType::Aero: case V6DataType::TyreState: case V6DataType::BrakeBias:
            return true;
        default: return false;
    }
}
int phaseRank(V6Phase phase) { return phase == V6Phase::Formation ? 0 : 1; }
bool before(V6Phase aPhase, float a, V6Phase bPhase, float b) {
    return phaseRank(aPhase) < phaseRank(bPhase) || (aPhase == bPhase && a < b);
}

std::vector<uint8_t> makeHeader(uint64_t metadataOffset, uint64_t metadataSize,
                                uint64_t directoryOffset, uint32_t chunkCount,
                                uint64_t footerOffset) {
    std::vector<uint8_t> out;
    out.insert(out.end(), MAGIC.begin(), MAGIC.end()); put16(out, 6); put16(out, HEADER_SIZE);
    put32(out, 3); put64(out, metadataOffset); put64(out, metadataSize);
    put64(out, directoryOffset); put32(out, chunkCount); put32(out, CHUNK_ENTRY_SIZE);
    put64(out, footerOffset);
    while (out.size() < 120) out.push_back(0);
    put32(out, static_cast<uint32_t>(::crc32(0, out.data(), 120)));
    while (out.size() < HEADER_SIZE) out.push_back(0);
    return out;
}

uint32_t controlCrc(std::string_view metadata, const std::vector<uint8_t>& directory) {
    uint32_t crc = static_cast<uint32_t>(::crc32(0,
        reinterpret_cast<const Bytef*>(metadata.data()), static_cast<uInt>(metadata.size())));
    return static_cast<uint32_t>(::crc32(crc, directory.data(), static_cast<uInt>(directory.size())));
}

uint32_t oldFamilyMask(V6DataType type) {
    switch (type) {
        case V6DataType::Speed: case V6DataType::RPM: case V6DataType::Gear:
        case V6DataType::Throttle: case V6DataType::Brake: case V6DataType::Steering:
        case V6DataType::TyreSurfaceTemp:
        case V6DataType::TyreInnerTemp: case V6DataType::BrakeTemp:
        case V6DataType::EngineTemp: return v4TypeBit(1);
        case V6DataType::Aero: return v4TypeBit(1) | v4TypeBit(2);
        case V6DataType::TyreState: return v4TypeBit(2) | v4TypeBit(10);
        case V6DataType::Fuel: case V6DataType::ERSStore:
        case V6DataType::ERSHarvest: case V6DataType::ERSDeployment:
        case V6DataType::EnginePower: case V6DataType::BrakeBias: return v4TypeBit(2);
        case V6DataType::TyreWear: case V6DataType::Damage: return v4TypeBit(3);
        case V6DataType::LapTiming: return v4TypeBit(4) | v4TypeBit(7);
        case V6DataType::GForce: return v4TypeBit(11);
        case V6DataType::RideHeight: return v4TypeBit(12);
        case V6DataType::Position: return v4TypeBit(13);
        default: return 0;
    }
}
bool requestedByOldMask(V6DataType type, uint32_t mask) {
    return mask == UINT32_MAX || (oldFamilyMask(type) & mask) != 0;
}
bool requestedForAllDrivers(V6DataType type, uint32_t mask) {
    switch (type) {
        case V6DataType::LapTiming: return (mask & v4TypeBit(7)) != 0;
        case V6DataType::Position: return (mask & v4TypeBit(13)) != 0;
        case V6DataType::Aero: case V6DataType::TyreState:
        case V6DataType::Fuel: case V6DataType::ERSStore:
        case V6DataType::ERSHarvest: case V6DataType::ERSDeployment:
        case V6DataType::EnginePower: case V6DataType::BrakeBias:
            return (mask & v4TypeBit(9)) != 0;
        default: return false;
    }
}
uint8_t sharedRowType(std::string_view json) {
    const auto type = rowType(json);
    if (type == "session") return 5;
    if (type == "race_event") return 6;
    if (type == "participants") return 8;
    return 0;
}

} // namespace

const char* v6TypeName(V6DataType type) {
    static constexpr const char* names[] = {"unknown","speed","rpm","gear","throttle","brake",
        "steering","aero","tyre_surface_temp","tyre_inner_temp","brake_temp","engine_temp",
        "tyre_wear","tyre_state","damage","fuel","ers_store","ers_harvest","ers_deployment",
        "engine_power","brake_bias","g_force","ride_height","position","lap_timing"};
    const auto value = static_cast<uint8_t>(type);
    return value < std::size(names) ? names[value] : names[0];
}
V6DataType v6TypeFromName(std::string_view name) {
    for (uint8_t value = 1; value < static_cast<uint8_t>(V6DataType::Count); ++value)
        if (name == v6TypeName(static_cast<V6DataType>(value))) return static_cast<V6DataType>(value);
    return V6DataType::Unknown;
}

struct TnrdV6Writer::Impl {
    struct Builder {
        std::string plain;
        float first{std::numeric_limits<float>::infinity()};
        float last{-std::numeric_limits<float>::infinity()};
        uint32_t count{};
    };
    struct PendingLap {
        V6LapSummary summary;
        std::map<V6DataType, Builder> chunks;
        V6Phase deadlinePhase{V6Phase::Race};
        float deadline{};
    };
    struct DriverState {
        bool known{};
        bool open{};
        bool seenActive{};
        // The car's race is over (retired, finished, disqualified). Its
        // telemetry is done, but the game keeps updating its classification,
        // so a LapTiming-only lap stays open for it. See terminate().
        bool terminal{};
        V6Phase terminalPhase{V6Phase::Race};
        float terminalTime{-1.0f};
        V6LapSummary current;
        std::map<V6DataType, Builder> chunks;
        std::vector<PendingLap> pending;
        std::map<V6DataType, std::map<std::string, std::string>> committedState;
        std::map<V6DataType, std::map<std::string, std::string>> lastState;
        std::set<V6DataType> committedUnavailable;
        std::set<V6DataType> unavailable;
        bool currentInvalid{};
    };
    struct PendingRestriction { uint8_t driver{}; V6RestrictionChange change; };
    struct PendingTyreHistory { uint8_t driver{}; V6Phase phase{}; float time{}; std::vector<V6TyreStintSummary> stints; };

    std::FILE* file{};
    std::string path;
    HeaderRow session;
    std::array<DriverState, 24> drivers;
    std::map<uint8_t, V6DriverHeader> liveHeaders;
    std::map<uint8_t, V6DriverHeader> committedHeaders;
    std::vector<V6LapSummary> committedLaps;
    std::vector<V6ChunkInfo> chunks;
    std::vector<V6Metadata::StoredShared> committedShared;
    std::vector<V6SharedRecord> pendingShared;
    std::vector<PendingRestriction> pendingRestrictions;
    std::vector<PendingTyreHistory> pendingTyreHistory;
    std::optional<uint8_t> player;
    uint32_t nextLapId{1};
    uint64_t nextSequence{1};
    V6Phase phase{V6Phase::Race};
    std::array<float, 2> phaseTime{{-1.0f, -1.0f}};
    std::array<float, 2> committedThrough{{-1.0f, -1.0f}};
    ZSTD_CCtx* compressor{};
    int compressionLevel{DEFAULT_COMPRESSION_LEVEL};
    std::vector<uint8_t> scratch;
    uint64_t chunkWrites{}, plainBytes{}, compressedBytes{}, compressionAllocated{}, checkpoints{};
    size_t lastPlain{}, lastCompressed{}, peakScratch{};

    ~Impl() { if (compressor) ZSTD_freeCCtx(compressor); }
    size_t phaseIndex(V6Phase value) const { return value == V6Phase::Formation ? 1 : 0; }
    float now() const { return phaseTime[phaseIndex(phase)]; }

    DriverState& ensure(uint8_t index, float time) {
        auto& state = drivers[index];
        state.known = true;
        auto [it, inserted] = liveHeaders.try_emplace(index);
        if (inserted) it->second.vehicleIndex = index;
        if (!state.open && !state.terminal) startLap(index, 0, time);
        return state;
    }
    // `resume` false keeps a car whose race is over in its terminal state, so
    // the lap being opened collects classification samples only.
    void startLap(uint8_t index, uint32_t number, float time, bool resume = true) {
        auto& state = drivers[index];
        state.open = true; state.current = {};
        if (resume) { state.terminal = false; state.terminalTime = -1.0f; }
        state.current.lapId = nextLapId++; state.current.driverIndex = index;
        state.current.lapNumber = number; state.current.phase = phase;
        state.current.startSessionTime = time; state.current.endSessionTime = time;
        state.current.isPartial = true; state.current.isValid = true;
        state.chunks.clear();
        for (const auto& [type, values] : state.lastState)
            for (const auto& [_, value] : values) add(index, type, time, retime(value, time), false);
        for (V6DataType type : state.unavailable)
            add(index, type, time, sample(time, {{"available", "false"}}), false);
    }
    void add(uint8_t index, V6DataType type, float time, std::string value, bool updateState = true) {
        if (index >= drivers.size() || type == V6DataType::Unknown || !std::isfinite(time)) return;
        // Once a car's race is over its telemetry is stale, but the game keeps
        // reclassifying it, so LapTiming is the one family that still records.
        if (drivers[index].terminal && type != V6DataType::LapTiming) return;
        if ((!player || index != *player) && !drivers[index].known) return;
        auto& state = ensure(index, time);
        if (!state.open) return;
        const bool missing = value.find("\"available\":false") != std::string::npos;
        if (missing) state.unavailable.insert(type); else state.unavailable.erase(type);
        if (updateState && stateType(type)) {
            const auto payload = [](const std::string& json) {
                const auto comma = json.find(','); return comma == std::string::npos ? std::string_view{} : std::string_view(json).substr(comma);
            };
            const auto comma = value.find(','); const auto colon = value.find(':', comma + 1);
            const std::string signature = colon == std::string::npos ? value : value.substr(comma + 1, colon - comma - 1);
            const auto typeState = state.lastState.find(type);
            if (typeState != state.lastState.end()) {
                const auto previous = typeState->second.find(signature);
                if (previous != typeState->second.end() && payload(previous->second) == payload(value)) return;
            }
            state.lastState[type][signature] = value;
        }
        auto& builder = state.chunks[type];
        builder.plain += value; builder.plain.push_back('\n');
        builder.first = std::min(builder.first, time); builder.last = std::max(builder.last, time); ++builder.count;
        state.current.endSessionTime = std::max(state.current.endSessionTime, time);
        liveHeaders[index].availableTypeMask |= v6DataTypeBit(type);
    }
    static Builder splitAt(Builder& source, float boundary) {
        Builder moved, kept; size_t at = 0;
        while (at < source.plain.size()) {
            auto end = source.plain.find('\n', at); if (end == std::string::npos) end = source.plain.size();
            if (end > at) {
                const std::string_view line(source.plain.data() + at, end - at); const float time = scanTime(line);
                auto& target = time >= boundary ? moved : kept;
                target.plain.append(line); target.plain.push_back('\n');
                target.first = std::min(target.first, time); target.last = std::max(target.last, time); ++target.count;
            }
            at = end + 1;
        }
        source = std::move(kept); return moved;
    }
    void boundary(uint8_t index, uint32_t newNumber, float time, uint32_t lapTime,
                  uint32_t s1, uint32_t s2, uint32_t s3, bool completed, bool valid) {
        auto& state = ensure(index, time);
        if (state.current.lapNumber == newNumber && state.current.phase == phase) return;
        std::map<V6DataType, Builder> moved;
        for (auto& [type, builder] : state.chunks) {
            auto tail = splitAt(builder, time); if (tail.count) moved.emplace(type, std::move(tail));
        }
        closeLap(index, time, lapTime, s1, s2, s3, completed, valid, false);
        startLap(index, newNumber, time);
        for (auto& [type, builder] : moved) state.chunks[type] = std::move(builder);
    }
    void closeLap(uint8_t index, float time, uint32_t lapTime, uint32_t s1, uint32_t s2,
                  uint32_t s3, bool completed, bool valid, bool formationTransition) {
        auto& state = drivers[index]; if (!state.open) return;
        state.current.endSessionTime = std::max(state.current.startSessionTime, time);
        state.current.lapTimeMs = lapTime; state.current.s1Ms = s1; state.current.s2Ms = s2;
        state.current.s3Ms = s3; state.current.isCompleted = completed;
        state.current.isValid = valid; state.current.isPartial = !completed;
        if (!state.chunks.empty() || state.current.lapNumber != 0) {
            PendingLap lap; lap.summary = state.current; lap.chunks = std::move(state.chunks);
            lap.deadlinePhase = formationTransition ? V6Phase::Race : phase;
            lap.deadline = formationTransition ? WRITE_DELAY : time + WRITE_DELAY;
            state.pending.push_back(std::move(lap));
        }
        state.open = false; state.chunks.clear();
    }
    // Ends a car's racing record. Its current lap is flushed, because a car
    // that has retired or finished produces no further telemetry worth
    // storing. Its classification is a different matter: the game demotes a
    // retirement through the order for a minute or more after the retirement
    // event, and writes the final result at session end. Keep a lap-number-0
    // lap open (invisible to the lap catalogs, which skip lap 0) so add() can
    // still record those LapTiming samples. Without it the archive freezes the
    // car at the position it held when it retired, and playback then shows it
    // sharing a position with a car still running.
    void terminate(uint8_t index, float time) {
        auto& state = drivers[index];
        if (!state.known || state.terminal) return;
        closeLap(index,time,0,0,0,0,false,!state.currentInvalid,false);
        state.terminal = true; state.terminalPhase = phase; state.terminalTime = time;
        startLap(index, 0, time, false);
    }
    bool eligible(const PendingLap& lap, bool force) const {
        return force || phaseRank(phase) > phaseRank(lap.deadlinePhase) ||
            (phase == lap.deadlinePhase && now() >= lap.deadline);
    }
    bool writeChunk(uint8_t driver, uint32_t lapId, V6Phase chunkPhase, V6DataType type,
                    const Builder& builder, V6ChunkInfo& info, std::string* errorOut) {
        if (!builder.count) return true;
        if (builder.plain.size() > MAX_CHUNK_PLAIN || chunks.size() >= MAX_CHUNKS) {
            fail(errorOut, "V6 chunk exceeds its format limit"); return false;
        }
        const size_t bound = ZSTD_compressBound(builder.plain.size());
        if (scratch.size() < bound) { scratch.resize(bound); compressionAllocated += scratch.capacity(); }
        if (!compressor) compressor = ZSTD_createCCtx();
        if (!compressor) { fail(errorOut, "could not allocate V6 compression context"); return false; }
        if (ZSTD_isError(ZSTD_CCtx_reset(compressor, ZSTD_reset_session_only)) ||
            ZSTD_isError(ZSTD_CCtx_setParameter(compressor, ZSTD_c_compressionLevel, compressionLevel)) ||
            ZSTD_isError(ZSTD_CCtx_setParameter(compressor, ZSTD_c_checksumFlag, 1))) {
            fail(errorOut, "could not configure V6 compression"); return false;
        }
        const size_t size = ZSTD_compress2(compressor, scratch.data(), scratch.size(),
                                           builder.plain.data(), builder.plain.size());
        if (ZSTD_isError(size)) { fail(errorOut, ZSTD_getErrorName(size)); return false; }
        if (!seekEnd(file)) { fail(errorOut, "could not append V6 chunk"); return false; }
        const uint64_t prefixOffset = tellFile(file);
        std::vector<uint8_t> prefix; put32(prefix, CHUNK_MAGIC); prefix.push_back(driver);
        prefix.push_back(static_cast<uint8_t>(type)); prefix.push_back(0);
        prefix.push_back(static_cast<uint8_t>(chunkPhase)); put32(prefix, lapId);
        put64(prefix, size); put64(prefix, builder.plain.size()); put32(prefix, builder.count);
        if (!writeAll(file, prefix.data(), prefix.size()) || !writeAll(file, scratch.data(), size)) {
            fail(errorOut, "failed while appending V6 chunk"); return false;
        }
        info = {driver, lapId, static_cast<uint8_t>(type), 0, chunkPhase,
                builder.first, builder.last, prefixOffset + CHUNK_PREFIX_SIZE,
                size, builder.plain.size(), builder.count,
                static_cast<uint32_t>(::crc32(0, reinterpret_cast<const Bytef*>(builder.plain.data()),
                                               static_cast<uInt>(builder.plain.size()))), nextSequence++};
        ++chunkWrites; plainBytes += builder.plain.size(); compressedBytes += size;
        lastPlain = builder.plain.size(); lastCompressed = size; peakScratch = std::max(peakScratch, scratch.capacity());
        return true;
    }

    bool writeShared(const V6SharedRecord& record, V6Metadata::StoredShared& info,
                     std::string* errorOut) {
        if (record.json.empty() || record.json.size() > MAX_CHUNK_PLAIN) {
            fail(errorOut, "V6 shared record exceeds its format limit"); return false;
        }
        const size_t bound = ZSTD_compressBound(record.json.size());
        if (scratch.size() < bound) { scratch.resize(bound); compressionAllocated += scratch.capacity(); }
        if (!compressor) compressor = ZSTD_createCCtx();
        if (!compressor || ZSTD_isError(ZSTD_CCtx_reset(compressor, ZSTD_reset_session_only)) ||
            ZSTD_isError(ZSTD_CCtx_setParameter(compressor, ZSTD_c_compressionLevel, compressionLevel)) ||
            ZSTD_isError(ZSTD_CCtx_setParameter(compressor, ZSTD_c_checksumFlag, 1))) {
            fail(errorOut, "could not configure V6 shared-record compression"); return false;
        }
        const size_t size = ZSTD_compress2(compressor, scratch.data(), scratch.size(),
                                           record.json.data(), record.json.size());
        if (ZSTD_isError(size) || !seekEnd(file)) {
            fail(errorOut, ZSTD_isError(size) ? ZSTD_getErrorName(size) : "could not append V6 shared record");
            return false;
        }
        const uint32_t checksum = static_cast<uint32_t>(::crc32(0,
            reinterpret_cast<const Bytef*>(record.json.data()), static_cast<uInt>(record.json.size())));
        const uint64_t prefixOffset = tellFile(file); std::vector<uint8_t> prefix;
        put32(prefix, SHARED_MAGIC); prefix.push_back(static_cast<uint8_t>(record.phase));
        while (prefix.size() < 8) prefix.push_back(0);
        put64(prefix, size); put64(prefix, record.json.size()); put32(prefix, checksum); put32(prefix, 0);
        if (!writeAll(file, prefix.data(), prefix.size()) || !writeAll(file, scratch.data(), size)) {
            fail(errorOut, "failed while appending V6 shared record"); return false;
        }
        info = {record.phase, record.sessionTime, prefixOffset + CHUNK_PREFIX_SIZE,
                size, record.json.size(), checksum};
        return true;
    }

    void applyParticipant(const ParticipantsRow& row, bool committed) {
        auto& target = committed ? committedHeaders : liveHeaders;
        for (const auto& driver : row.drivers) {
            if (driver.idx < 0 || driver.idx >= 24) continue;
            auto& header = target[static_cast<uint8_t>(driver.idx)]; header.vehicleIndex = driver.idx;
            if (header.driverName.empty()) { header.driverName = driver.name; header.teamId = driver.team_id; header.raceNumber = driver.race_number; }
            header.isPlayer = driver.idx == row.player_idx;
            const auto setting = !driver.your_telemetry ? TelemetrySetting::Unknown
                : (*driver.your_telemetry == 1 ? TelemetrySetting::Public : TelemetrySetting::Restricted);
            if (header.initialTelemetrySetting == TelemetrySetting::Unknown)
                header.initialTelemetrySetting = setting;
        }
    }
    void rebuildLiveMetadata() {
        liveHeaders = committedHeaders;
        for (const auto& record : pendingShared) if (rowType(record.json) == "participants") {
            ParticipantsRow row; if (!glz::read<kPartialRead>(row, record.json)) applyParticipant(row, false);
        }
        for (const auto& pending : pendingRestrictions) {
            auto& header = liveHeaders[pending.driver]; header.vehicleIndex = pending.driver;
            if (header.initialTelemetrySetting == TelemetrySetting::Unknown)
                header.initialTelemetrySetting = pending.change.setting;
            else header.restrictionChanges.push_back(pending.change);
        }
        for (const auto& pending : pendingTyreHistory)
            liveHeaders[pending.driver].tyreStints = pending.stints;
        for (uint8_t i = 0; i < drivers.size(); ++i) {
            auto& state = drivers[i]; if (!state.known) continue;
            auto& header = liveHeaders[i]; header.vehicleIndex = i;
            for (const auto& [type, _] : state.committedState) header.availableTypeMask |= v6DataTypeBit(type);
            for (const auto& lap : state.pending) for (const auto& [type, _] : lap.chunks) header.availableTypeMask |= v6DataTypeBit(type);
            for (const auto& [type, _] : state.chunks) header.availableTypeMask |= v6DataTypeBit(type);
        }
    }
    bool commitControl(bool force, std::string* errorOut) {
        auto shared = pendingShared.begin();
        while (shared != pendingShared.end()) {
            const bool ready = force || phaseRank(phase) > phaseRank(shared->phase) ||
                (phase == shared->phase && shared->sessionTime < now() - WRITE_DELAY);
            if (!ready) { ++shared; continue; }
            if (rowType(shared->json) == "participants") {
                ParticipantsRow row; if (!glz::read<kPartialRead>(row, shared->json)) applyParticipant(row, true);
            }
            V6Metadata::StoredShared stored;
            if (!writeShared(*shared, stored, errorOut)) return false;
            committedThrough[phaseIndex(shared->phase)] = std::max(committedThrough[phaseIndex(shared->phase)], shared->sessionTime);
            committedShared.push_back(stored); shared = pendingShared.erase(shared);
        }
        auto restriction = pendingRestrictions.begin();
        while (restriction != pendingRestrictions.end()) {
            const auto& change = restriction->change;
            const bool ready = force || phaseRank(phase) > phaseRank(change.phase) ||
                (phase == change.phase && change.sessionTime < now() - WRITE_DELAY);
            if (!ready) { ++restriction; continue; }
            auto& header = committedHeaders[restriction->driver]; header.vehicleIndex = restriction->driver;
            if (header.initialTelemetrySetting == TelemetrySetting::Unknown)
                header.initialTelemetrySetting = change.setting;
            else if (header.restrictionChanges.empty() || header.restrictionChanges.back().setting != change.setting)
                header.restrictionChanges.push_back(change);
            committedThrough[phaseIndex(change.phase)] = std::max(
                committedThrough[phaseIndex(change.phase)], change.sessionTime);
            restriction = pendingRestrictions.erase(restriction);
        }
        auto history = pendingTyreHistory.begin();
        while (history != pendingTyreHistory.end()) {
            const bool ready = force || phaseRank(phase) > phaseRank(history->phase) ||
                (phase == history->phase && history->time < now() - WRITE_DELAY);
            if (!ready) { ++history; continue; }
            auto& header = committedHeaders[history->driver]; header.vehicleIndex = history->driver;
            header.tyreStints = history->stints;
            committedThrough[phaseIndex(history->phase)] = std::max(
                committedThrough[phaseIndex(history->phase)], history->time);
            history = pendingTyreHistory.erase(history);
        }
        return true;
    }
    // True when some pending item has reached its write deadline. Nothing in
    // the writer's live state depends on a commit that would do no work:
    // participant names and telemetry-restriction changes are applied to
    // liveHeaders as they arrive (see appendRow), so skipping an idle commit
    // leaves exactly the state a commit would have rebuilt. advanceSessionTime
    // runs this for every datagram, so it must stay cheap.
    bool commitDue() const {
        for (uint8_t index = 0; index < drivers.size(); ++index)
            for (const auto& lap : drivers[index].pending)
                if (eligible(lap, false)) return true;
        const float time = now();
        for (const auto& record : pendingShared)
            if (phaseRank(phase) > phaseRank(record.phase) ||
                (phase == record.phase && record.sessionTime < time - WRITE_DELAY)) return true;
        for (const auto& pending : pendingRestrictions)
            if (phaseRank(phase) > phaseRank(pending.change.phase) ||
                (phase == pending.change.phase && pending.change.sessionTime < time - WRITE_DELAY)) return true;
        for (const auto& pending : pendingTyreHistory)
            if (phaseRank(phase) > phaseRank(pending.phase) ||
                (phase == pending.phase && pending.time < time - WRITE_DELAY)) return true;
        return false;
    }

    bool commit(bool force, std::string* errorOut) {
        if (!force && !commitDue()) return true;
        const size_t chunksBefore = chunks.size();
        for (uint8_t index = 0; index < drivers.size(); ++index) {
            auto& state = drivers[index]; auto lap = state.pending.begin();
            while (lap != state.pending.end()) {
                if (!eligible(*lap, force)) { ++lap; continue; }
                std::vector<V6ChunkInfo> written; written.reserve(lap->chunks.size());
                for (const auto& [type, builder] : lap->chunks) {
                    V6ChunkInfo info; if (!writeChunk(index, lap->summary.lapId, lap->summary.phase,
                                                       type, builder, info, errorOut)) return false;
                    if (builder.count) written.push_back(info);
                }
                chunks.insert(chunks.end(), written.begin(), written.end());
                committedLaps.push_back(lap->summary);
                auto& header = committedHeaders[index]; header.vehicleIndex = index;
                header.lapIds.push_back(lap->summary.lapId);
                for (const auto& [type, builder] : lap->chunks) {
                    if (builder.count) header.availableTypeMask |= v6DataTypeBit(type);
                    if (stateType(type) && builder.count) {
                        size_t start = 0;
                        while (start < builder.plain.size()) {
                            size_t end = builder.plain.find('\n', start); if (end == std::string::npos) end = builder.plain.size();
                            if (end > start) {
                                const std::string value = builder.plain.substr(start, end - start);
                                const auto comma = value.find(','); const auto colon = value.find(':', comma + 1);
                                const std::string signature = colon == std::string::npos ? value : value.substr(comma + 1, colon - comma - 1);
                                state.committedState[type][signature] = value;
                                if (value.find("\"available\":false") != std::string::npos)
                                    state.committedUnavailable.insert(type);
                                else state.committedUnavailable.erase(type);
                            }
                            start = end + 1;
                        }
                    }
                }
                committedThrough[phaseIndex(lap->summary.phase)] = std::max(
                    committedThrough[phaseIndex(lap->summary.phase)], lap->summary.endSessionTime);
                lap = state.pending.erase(lap);
            }
        }
        if (!commitControl(force, errorOut)) return false;
        rebuildLiveMetadata();
        (void)chunksBefore;
        // No checkpoint here. The index is written once by finish(), and a
        // recording interrupted before that is rebuilt by the reader's
        // recovery scan. See docs/TNRD_V6_WRITER_EFFICIENCY_DESIGN.md.
        return true;
    }

    bool snapshotTo(std::FILE* output, const std::vector<V6ChunkInfo>& directoryChunks,
                    const std::vector<V6Metadata::StoredShared>& sharedRecords,
                    bool countCheckpoint, std::string* errorOut) {
        V6Metadata metadata; metadata.session = session;
        for (const auto& [_, header] : committedHeaders) metadata.drivers.push_back(header);
        metadata.laps = committedLaps; metadata.shared = sharedRecords;
        const std::string metadataJson = jsonOf(metadata);
        if (metadataJson.empty() || metadataJson.size() > MAX_METADATA_BYTES) {
            fail(errorOut, "V6 metadata exceeds its format limit"); return false;
        }
        if (!seekEnd(output)) { fail(errorOut, "could not append V6 checkpoint"); return false; }
        const uint64_t metadataOffset = tellFile(output);
        if (!writeAll(output, metadataJson.data(), metadataJson.size())) { fail(errorOut, "could not write V6 metadata"); return false; }
        const uint64_t directoryOffset = tellFile(output);
        std::vector<uint8_t> directory; directory.reserve(directoryChunks.size() * CHUNK_ENTRY_SIZE);
        for (const auto& chunk : directoryChunks) {
            directory.push_back(chunk.driverIndex); directory.push_back(chunk.typeId);
            directory.push_back(chunk.flags); directory.push_back(static_cast<uint8_t>(chunk.phase));
            put32(directory, chunk.lapId); putFloat(directory, chunk.firstTime); putFloat(directory, chunk.lastTime);
            put64(directory, chunk.offset); put64(directory, chunk.compressedSize);
            put64(directory, chunk.uncompressedSize); put32(directory, chunk.sampleCount);
            put32(directory, chunk.checksum); put64(directory, chunk.sequence);
        }
        if (!writeAll(output, directory.data(), directory.size())) { fail(errorOut, "could not write V6 directory"); return false; }
        const uint64_t footerOffset = tellFile(output); std::vector<uint8_t> footer;
        put32(footer, FOOTER_MAGIC); put16(footer, 6); put16(footer, FOOTER_SIZE);
        put64(footer, metadataOffset); put64(footer, directoryOffset);
        put32(footer, static_cast<uint32_t>(directoryChunks.size())); put32(footer, static_cast<uint32_t>(metadataJson.size()));
        put32(footer, controlCrc(metadataJson, directory)); while (footer.size() < FOOTER_SIZE) footer.push_back(0);
        if (!writeAll(output, footer.data(), footer.size()) || std::fflush(output) != 0) {
            fail(errorOut, "could not commit V6 footer"); return false;
        }
        const auto header = makeHeader(metadataOffset, metadataJson.size(), directoryOffset,
                                       static_cast<uint32_t>(directoryChunks.size()), footerOffset);
        if (!seekFile(output, 0) || !writeAll(output, header.data(), header.size()) || std::fflush(output) != 0) {
            fail(errorOut, "could not commit V6 header"); return false;
        }
        if (countCheckpoint) ++checkpoints;
        return seekEnd(output);
    }

    bool snapshot(std::string* errorOut) {
        return snapshotTo(file, chunks, committedShared, true, errorOut);
    }

    // Writes the session HeaderRow as an uncompressed SES6 record directly
    // after the file header. A recording interrupted before its index is
    // written has no metadata JSON, so this is the only place the track,
    // session type and protocol survive for the recovery scan to find.
    bool writeSessionRecord(std::string* errorOut) {
        const std::string json = jsonOf(session);
        if (json.empty() || json.size() > MAX_METADATA_BYTES) {
            fail(errorOut, "V6 session record exceeds its format limit"); return false;
        }
        std::vector<uint8_t> prefix; put32(prefix, SESSION_MAGIC);
        while (prefix.size() < 8) prefix.push_back(0);
        put64(prefix, json.size()); put64(prefix, json.size());
        while (prefix.size() < CHUNK_PREFIX_SIZE) prefix.push_back(0);
        if (!seekEnd(file) || !writeAll(file, prefix.data(), prefix.size()) ||
            !writeAll(file, json.data(), json.size()) || std::fflush(file) != 0) {
            fail(errorOut, "could not write V6 session record"); return false;
        }
        return true;
    }

    TelemetrySetting setting(uint8_t index) const {
        const auto it = liveHeaders.find(index); if (it == liveHeaders.end()) return TelemetrySetting::Unknown;
        return it->second.restrictionChanges.empty() ? it->second.initialTelemetrySetting
                                                      : it->second.restrictionChanges.back().setting;
    }
    bool privateAvailable(uint8_t index) const { return player == index || setting(index) == TelemetrySetting::Public; }
    void unavailable(uint8_t index, float time) {
        const std::string missing = sample(time, {{"available", "false"}});
        for (auto type : {V6DataType::Fuel,V6DataType::ERSStore,V6DataType::ERSHarvest,
                          V6DataType::ERSDeployment,V6DataType::EnginePower,V6DataType::BrakeBias,
                          V6DataType::TyreWear,V6DataType::Damage,V6DataType::TyreState}) {
            drivers[index].lastState[type].clear();
            add(index, type, time, missing);
        }
    }

    // Implemented after the row-specific helpers below.
    bool appendRow(std::string_view, float, std::string*);
    bool rewind(float, std::string*);
};

bool TnrdV6Writer::Impl::appendRow(std::string_view json, float suppliedTime, std::string* errorOut) {
    if (json.empty()) return true;
    const std::string kind = rowType(json);
    float time = suppliedTime >= 0.0f ? suppliedTime : scanTime(json);
    if (!std::isfinite(time) || time < 0.0f) time = std::max(0.0f, now());
    phaseTime[phaseIndex(phase)] = std::max(phaseTime[phaseIndex(phase)], time);

    auto telemetry = [&](uint8_t index, const auto& car) {
        add(index, V6DataType::Speed, time, sample(time, {{"speed_kph",integer(car.speed_kph)}}));
        std::vector<std::pair<std::string,std::string>> rpm{{"rpm",integer(car.rpm)}};
        if constexpr (requires { car.rev_lights_pct.has_value(); }) {
            if (car.rev_lights_pct) rpm.emplace_back("rev_lights_pct", integer(*car.rev_lights_pct));
            if (car.rev_lights_bit_value) rpm.emplace_back("rev_lights_bit_value", integer(*car.rev_lights_bit_value));
        } else {
            rpm.emplace_back("rev_lights_pct", integer(car.rev_lights_pct));
            rpm.emplace_back("rev_lights_bit_value", integer(car.rev_lights_bit_value));
        }
        add(index, V6DataType::RPM, time, sample(time, rpm));
        add(index, V6DataType::Gear, time, sample(time, {{"gear",integer(car.gear)}}));
        if constexpr (requires { car.throttle.has_value(); }) {
            if (car.throttle) add(index,V6DataType::Throttle,time,sample(time,{{"throttle",number(*car.throttle)}}));
            if (car.brake) add(index,V6DataType::Brake,time,sample(time,{{"brake",number(*car.brake)}}));
            if (car.steering) add(index,V6DataType::Steering,time,sample(time,{{"steering",number(*car.steering)}}));
        } else {
            add(index,V6DataType::Throttle,time,sample(time,{{"throttle",number(car.throttle)}}));
            add(index,V6DataType::Brake,time,sample(time,{{"brake",number(car.brake)}}));
            add(index,V6DataType::Steering,time,sample(time,{{"steering",number(car.steering)}}));
        }
        add(index,V6DataType::Aero,time,sample(time,{{"drs",integer(car.drs)},{"slm",integer(car.slm)}}));
        add(index,V6DataType::TyreSurfaceTemp,time,sample(time,{
            {"tyre_temp_surface_fl",integer(car.tyre_temp_surface_fl)},
            {"tyre_temp_surface_fr",integer(car.tyre_temp_surface_fr)},
            {"tyre_temp_surface_rl",integer(car.tyre_temp_surface_rl)},
            {"tyre_temp_surface_rr",integer(car.tyre_temp_surface_rr)}}));
        add(index,V6DataType::TyreInnerTemp,time,sample(time,{
            {"tyre_temp_inner_fl",integer(car.tyre_temp_inner_fl)},
            {"tyre_temp_inner_fr",integer(car.tyre_temp_inner_fr)},
            {"tyre_temp_inner_rl",integer(car.tyre_temp_inner_rl)},
            {"tyre_temp_inner_rr",integer(car.tyre_temp_inner_rr)}}));
        add(index,V6DataType::BrakeTemp,time,sample(time,{
            {"brake_temp_fl",integer(car.brake_temp_fl)},{"brake_temp_fr",integer(car.brake_temp_fr)},
            {"brake_temp_rl",integer(car.brake_temp_rl)},{"brake_temp_rr",integer(car.brake_temp_rr)}}));
        add(index,V6DataType::EngineTemp,time,sample(time,{{"engine_temp",integer(car.engine_temp)}}));
    };

    if (kind == "telemetry") {
        TelemetryRow row; if (glz::read<kPartialRead>(row, json)) return true;
        if (row.player_idx >= 0 && row.player_idx < 24) {
            player = static_cast<uint8_t>(row.player_idx); telemetry(*player, row);
        }
        if (row.cars) for (const auto& car : *row.cars)
            if (car.idx >= 0 && car.idx < 24 && (!player || car.idx != *player)) telemetry(static_cast<uint8_t>(car.idx), car);
    } else if (kind == "positions") {
        PositionsRow row; if (glz::read<kPartialRead>(row, json)) return true;
        if (row.player_idx >= 0 && row.player_idx < 24) player = static_cast<uint8_t>(row.player_idx);
        for (const auto& car : row.cars) if (car.idx >= 0 && car.idx < 24) {
            const auto index = static_cast<uint8_t>(car.idx);
            add(index,V6DataType::Position,time,sample(time,{{"x",number(car.x)},{"z",number(car.z)}}));
            if (car.g_lat && car.g_long && car.g_vert)
                add(index,V6DataType::GForce,time,sample(time,{{"g_lat",number(*car.g_lat)},
                    {"g_long",number(*car.g_long)},{"g_vert",number(*car.g_vert)}}));
        }
    } else if (kind == "motion") {
        MotionRow row; if (!glz::read<kPartialRead>(row, json) && row.player_idx >= 0 && row.player_idx < 24) {
            player = static_cast<uint8_t>(row.player_idx);
            add(*player,V6DataType::GForce,time,sample(time,{{"g_lat",number(row.g_lat)},
                {"g_long",number(row.g_long)},{"g_vert",number(row.g_vert)}}));
        }
    } else if (kind == "motion_ex") {
        MotionExRow row; if (!glz::read<kPartialRead>(row, json) && row.player_idx >= 0 && row.player_idx < 24) {
            player = static_cast<uint8_t>(row.player_idx);
            add(*player,V6DataType::RideHeight,time,sample(time,{
                {"front_aero_height_mm",number(row.front_aero_height_mm)},
                {"rear_aero_height_mm",number(row.rear_aero_height_mm)}}));
        }
    } else if (kind == "timing") {
        TimingRow row; if (glz::read<kPartialRead>(row, json)) return true;
        if (row.player_idx >= 0 && row.player_idx < 24) player = static_cast<uint8_t>(row.player_idx);
        for (const auto& car : row.cars) if (car.idx >= 0 && car.idx < 24) {
            const uint8_t index = static_cast<uint8_t>(car.idx);
            auto& existing = drivers[index];
            const bool active = car.result_status == 2;
            const bool ended = (car.result_status >= 3 && car.result_status <= 7) ||
                (car.result_status == 1 && existing.seenActive);
            if (!active && !ended) continue;
            auto& state = ensure(index, time);
            if (!state.open) continue;
            if (active) state.seenActive = true;
            // A car whose race is over is on its classification lap: its racing
            // laps are closed and its lap number no longer advances, so only
            // the standings sample below still applies to it.
            if (!state.terminal) {
                const uint32_t lapNumber = car.lap_num > 0 ? static_cast<uint32_t>(car.lap_num) : 0;
                if (state.current.lapNumber != lapNumber || state.current.phase != phase) {
                    const uint32_t s1 = car.s1_ms > 0 ? static_cast<uint32_t>(car.s1_ms) : 0;
                    const uint32_t s2 = car.s2_ms > 0 ? static_cast<uint32_t>(car.s2_ms) : 0;
                    const uint32_t total = car.last_lap_ms > 0 ? static_cast<uint32_t>(car.last_lap_ms) : 0;
                    const uint32_t s3 = total > s1 + s2 ? total - s1 - s2 : 0;
                    boundary(index, lapNumber, time, total, s1, s2, s3,
                             lapNumber > state.current.lapNumber && total > 0,
                             !state.currentInvalid);
                }
                state.currentInvalid = car.lap_invalid;
            }
            std::vector<std::pair<std::string,std::string>> values{
                {"position",integer(car.position)},{"lap_num",integer(car.lap_num)},
                {"current_lap_ms",integer(car.current_lap_ms)},{"last_lap_ms",integer(car.last_lap_ms)},
                {"s1_ms",integer(car.s1_ms)},{"s2_ms",integer(car.s2_ms)},{"gap_ms",integer(car.gap_ms)},
                {"pit_status",integer(car.pit_status)},{"num_pit_stops",integer(car.num_pit_stops)},
                {"lap_invalid",boolean(car.lap_invalid)},{"penalties_s",integer(car.penalties_s)},
                {"num_dt_pens",integer(car.num_dt_pens)},{"num_sg_pens",integer(car.num_sg_pens)},
                {"sector",integer(car.sector)},{"result_status",integer(car.result_status)},
                {"driver_status",integer(car.driver_status)}};
            if (car.lap_distance_m) values.emplace_back("lap_distance_m", number(*car.lap_distance_m));
            add(index,V6DataType::LapTiming,time,sample(time,values));
            if (ended) terminate(index,time);
        }
    } else if (kind == "all_status") {
        AllStatusRow row; if (glz::read<kPartialRead>(row, json)) return true;
        for (const auto& car : row.cars) if (car.idx >= 0 && car.idx < 24) {
            const uint8_t index = static_cast<uint8_t>(car.idx);
            add(index,V6DataType::Aero,time,sample(time,{{"drs_allowed",boolean(car.drs_allowed)}}));
            add(index,V6DataType::TyreState,time,sample(time,{{"tyre_compound",integer(car.tyre_compound)},
                {"visual_compound",integer(car.visual_compound)},{"tyre_age_laps",integer(car.tyre_age_laps)}}));
            if (!privateAvailable(index)) continue;
            add(index,V6DataType::Fuel,time,sample(time,{{"fuel_kg",number(car.fuel_kg)},
                {"fuel_laps",number(car.fuel_laps)},{"fuel_mix",integer(car.fuel_mix)}}));
            add(index,V6DataType::BrakeBias,time,sample(time,{{"front_brake_bias",integer(car.front_brake_bias)}}));
            add(index,V6DataType::ERSStore,time,sample(time,{{"ers_j",integer(car.ers_j)},
                {"ers_pct",number(car.ers_pct)},{"ers_mode",integer(car.ers_mode)}}));
            add(index,V6DataType::ERSHarvest,time,sample(time,{{"ers_harvested_mguk_j",integer(car.ers_harvested_mguk_j)},
                {"ers_harvested_mguh_j",integer(car.ers_harvested_mguh_j)}}));
            add(index,V6DataType::ERSDeployment,time,sample(time,{{"ers_deployed_j",integer(car.ers_deployed_j)}}));
            add(index,V6DataType::EnginePower,time,sample(time,{{"engine_power_ice_kw",number(car.engine_power_ice_kw)},
                {"engine_power_mguk_kw",number(car.engine_power_mguk_kw)}}));
        }
    } else if (kind == "damage") {
        DamageRow row; if (glz::read<kPartialRead>(row, json)) return true;
        if (row.player_idx >= 0 && row.player_idx < 24) player = static_cast<uint8_t>(row.player_idx);
        auto damage = [&](uint8_t index, const auto& car, bool full) {
            std::vector<std::pair<std::string,std::string>> wear, values;
#define ADD_OPT(target, field) if constexpr (requires { car.field.has_value(); }) { if (car.field) target.emplace_back(#field, number(*car.field)); } else if (full) target.emplace_back(#field, number(car.field))
            ADD_OPT(wear,tyre_wear_fl); ADD_OPT(wear,tyre_wear_fr); ADD_OPT(wear,tyre_wear_rl); ADD_OPT(wear,tyre_wear_rr);
            ADD_OPT(values,tyre_dmg_fl); ADD_OPT(values,tyre_dmg_fr); ADD_OPT(values,tyre_dmg_rl); ADD_OPT(values,tyre_dmg_rr);
            ADD_OPT(values,brake_dmg_fl); ADD_OPT(values,brake_dmg_fr); ADD_OPT(values,brake_dmg_rl); ADD_OPT(values,brake_dmg_rr);
            ADD_OPT(values,blisters_fl); ADD_OPT(values,blisters_fr); ADD_OPT(values,blisters_rl); ADD_OPT(values,blisters_rr);
            ADD_OPT(values,wing_fl); ADD_OPT(values,wing_fr); ADD_OPT(values,wing_rear); ADD_OPT(values,floor_damage);
            ADD_OPT(values,diffuser_damage); ADD_OPT(values,sidepod_damage); ADD_OPT(values,gearbox_damage);
            ADD_OPT(values,engine_damage); ADD_OPT(values,drs_fault); ADD_OPT(values,ers_fault);
#undef ADD_OPT
            if (!wear.empty()) add(index,V6DataType::TyreWear,time,sample(time,wear));
            if (!values.empty()) add(index,V6DataType::Damage,time,sample(time,values));
        };
        if (player) damage(*player,row,true);
        if (row.cars) for (const auto& car : *row.cars)
            if (car.idx >= 0 && car.idx < 24 && (!player || car.idx != *player))
                damage(static_cast<uint8_t>(car.idx),car,false);
    } else if (kind == "tyre_sets") {
        TyreSetsRow row; if (!glz::read<kPartialRead>(row, json) && row.car_idx >= 0 && row.car_idx < 24) {
            const uint8_t index = static_cast<uint8_t>(row.car_idx);
            if (privateAvailable(index)) add(index,V6DataType::TyreState,time,sample(time,
                {{"sets",jsonOf(row.sets)},{"fitted_idx",integer(row.fitted_idx)}}));
        }
    } else if (kind == "participants") {
        ParticipantsRow row; if (glz::read<kPartialRead>(row, json)) return true;
        if (row.player_idx >= 0 && row.player_idx < 24) player = static_cast<uint8_t>(row.player_idx);
        for (const auto& driver : row.drivers) if (driver.idx >= 0 && driver.idx < 24) {
            const uint8_t index = static_cast<uint8_t>(driver.idx); ensure(index,time);
            auto& header = liveHeaders[index]; header.vehicleIndex = index;
            header.driverName = driver.name; header.teamId = driver.team_id; header.raceNumber = driver.race_number;
            header.isPlayer = player == index;
            const auto next = !driver.your_telemetry ? TelemetrySetting::Unknown
                : (*driver.your_telemetry == 1 ? TelemetrySetting::Public : TelemetrySetting::Restricted);
            const auto previous = setting(index);
            if (next != TelemetrySetting::Unknown && next != previous) {
                const V6RestrictionChange change{phase,time,next};
                pendingRestrictions.push_back({index,change});
                if (header.initialTelemetrySetting == TelemetrySetting::Unknown) header.initialTelemetrySetting = next;
                else header.restrictionChanges.push_back(change);
                if (previous == TelemetrySetting::Public && next == TelemetrySetting::Restricted && player != index)
                    unavailable(index,time);
            }
        }
        pendingShared.push_back({phase,time,std::string(json)});
    } else if (kind == "session") {
        SessionRow row; if (!glz::read<kPartialRead>(row,json)) {
            if (row.safety_car_status == 3 && phase != V6Phase::Formation && phaseTime[1] < 0.0f) {
                phase = V6Phase::Formation; phaseTime[1] = time;
                // Participants and the first timing samples commonly precede
                // the first Session packet. They are still formation data; all
                // of it is uncommitted at this point, so move that provisional
                // prefix into the formation phase before the game resets its
                // session clock for race lap 1.
                for (auto& record : pendingShared) record.phase = phase;
                for (auto& restriction : pendingRestrictions) restriction.change.phase = phase;
                for (auto& history : pendingTyreHistory) history.phase = phase;
                for (auto& state : drivers) {
                    if (state.open) state.current.phase = phase;
                    for (auto& lap : state.pending) lap.summary.phase = phase;
                }
            }
            pendingShared.push_back({phase,time,std::string(json)});
        }
    } else if (kind == "race_event") {
        RaceEventRow row; if (glz::read<kPartialRead>(row,json)) return true;
        if (row.code == "SCAR" && row.safety_car_type == 3 && row.event_type == 3 && phase == V6Phase::Formation) {
            const float formationEnd = std::max(0.0f, phaseTime[1]);
            for (uint8_t index = 0; index < drivers.size(); ++index)
                if (drivers[index].open) closeLap(index,formationEnd,0,0,0,0,false,!drivers[index].currentInvalid,true);
            phase = V6Phase::Race; phaseTime[0] = time;
            for (uint8_t index = 0; index < drivers.size(); ++index)
                if (drivers[index].known) startLap(index,1,time);
        }
        if (row.code == "RTMT" && row.car_idx && *row.car_idx >= 0 && *row.car_idx < 24)
            terminate(static_cast<uint8_t>(*row.car_idx),time);
        pendingShared.push_back({phase,time,std::string(json)});
    } else if (kind == "session_history_fastest") {
        SessionHistoryFastestRow row; if (!glz::read<kPartialRead>(row,json) && row.car_idx >= 0 && row.car_idx < 24) {
            auto& state = drivers[row.car_idx];
            for (const auto& history : row.laps) for (auto& lap : state.pending)
                if (lap.summary.lapNumber == static_cast<uint32_t>(history.lap_num)) {
                    lap.summary.lapTimeMs = history.lap_time_ms; lap.summary.s1Ms = history.s1_ms;
                    lap.summary.s2Ms = history.s2_ms; lap.summary.s3Ms = history.s3_ms;
                    lap.summary.isCompleted = history.lap_time_ms > 0; lap.summary.isPartial = !lap.summary.isCompleted;
                    lap.summary.isValid = history.lap_valid; break;
                }
            std::vector<V6TyreStintSummary> stints; stints.reserve(row.tyre_stints.size());
            for (const auto& stint : row.tyre_stints)
                stints.push_back({stint.end_lap, stint.actual_compound, stint.visual_compound});
            pendingTyreHistory.erase(std::remove_if(pendingTyreHistory.begin(), pendingTyreHistory.end(),
                [&](const auto& value) { return value.driver == row.car_idx && value.phase == phase; }), pendingTyreHistory.end());
            pendingTyreHistory.push_back({static_cast<uint8_t>(row.car_idx), phase, time, std::move(stints)});
        }
    }
    return commit(false,errorOut);
}

bool TnrdV6Writer::Impl::rewind(float time, std::string* errorOut) {
    if (!std::isfinite(time) || time < 0.0f) { fail(errorOut,"invalid V6 rewind target"); return false; }
    if (time <= committedThrough[phaseIndex(phase)]) {
        fail(errorOut,"unsupported rewind overlaps committed V6 data"); return false;
    }
    phaseTime[phaseIndex(phase)] = time;
    for (auto& state : drivers) {
        if (!state.known) continue;
        if (state.terminal && state.terminalPhase == phase && state.terminalTime >= time) {
            state.terminal = false;
            state.terminalTime = -1.0f;
        }
        auto pending = state.pending.begin(); bool reopened = false;
        while (pending != state.pending.end()) {
            if (pending->summary.phase != phase) { ++pending; continue; }
            if (pending->summary.startSessionTime >= time) { pending = state.pending.erase(pending); continue; }
            if (time <= pending->summary.endSessionTime) {
                state.current = pending->summary; state.current.endSessionTime = time;
                state.current.isCompleted = false; state.current.isPartial = true;
                state.chunks = std::move(pending->chunks); state.open = true; reopened = true;
                pending = state.pending.erase(pending);
                for (auto& [_, builder] : state.chunks) (void)splitAt(builder,time);
                continue;
            }
            ++pending;
        }
        if (!reopened && state.open && state.current.phase == phase) {
            if (state.current.startSessionTime >= time) { state.chunks.clear(); state.current.startSessionTime = time; }
            for (auto& [_, builder] : state.chunks) (void)splitAt(builder,time);
            state.current.endSessionTime = time;
        }
        state.lastState = state.committedState;
        state.unavailable = state.committedUnavailable;
        auto consider = [&](const std::map<V6DataType,Builder>& chunks) {
            for (const auto& [type,builder] : chunks) if (stateType(type) && builder.count) {
                size_t start = 0;
                while (start < builder.plain.size()) {
                    size_t end = builder.plain.find('\n', start); if (end == std::string::npos) end = builder.plain.size();
                    if (end > start) {
                        const std::string value = builder.plain.substr(start,end-start);
                        const auto comma = value.find(','); const auto colon = value.find(':', comma + 1);
                        const std::string signature = colon == std::string::npos ? value : value.substr(comma + 1, colon - comma - 1);
                        state.lastState[type][signature] = value;
                        if (value.find("\"available\":false") != std::string::npos)
                            state.unavailable.insert(type);
                        else state.unavailable.erase(type);
                    }
                    start = end + 1;
                }
            }
        };
        for (const auto& lap : state.pending) consider(lap.chunks); consider(state.chunks);
    }
    pendingShared.erase(std::remove_if(pendingShared.begin(),pendingShared.end(),[&](const auto& row) {
        return row.phase == phase && row.sessionTime >= time;
    }),pendingShared.end());
    pendingRestrictions.erase(std::remove_if(pendingRestrictions.begin(),pendingRestrictions.end(),[&](const auto& row) {
        return row.change.phase == phase && row.change.sessionTime >= time;
    }),pendingRestrictions.end());
    pendingTyreHistory.erase(std::remove_if(pendingTyreHistory.begin(),pendingTyreHistory.end(),[&](const auto& row) {
        return row.phase == phase && row.time >= time;
    }),pendingTyreHistory.end());
    rebuildLiveMetadata(); return true;
}

TnrdV6Writer::TnrdV6Writer() : impl_(std::make_unique<Impl>()) {}
TnrdV6Writer::~TnrdV6Writer() { if (isOpen()) { std::string ignored; (void)finish(&ignored); } }
bool TnrdV6Writer::isOpen() const { return impl_ && impl_->file; }
bool TnrdV6Writer::open(const std::string& path, const HeaderRow& header, std::string* errorOut) {
    if (isOpen()) { fail(errorOut,"V6 writer is already open"); return false; }
    // open() starts a fresh Impl, so anything configured on the writer before
    // it has to survive the swap. The compression level is set once, ahead of
    // the first stream, and would otherwise silently revert to the default.
    const int level = impl_ ? impl_->compressionLevel : DEFAULT_COMPRESSION_LEVEL;
    impl_ = std::make_unique<Impl>(); impl_->compressionLevel = level;
    impl_->path = path; impl_->file = openTnrdFile(path,"w+b");
    if (!impl_->file) { fail(errorOut,"could not create V6 file: " + std::string(std::strerror(errno))); return false; }
    impl_->session = header; impl_->session.magic = "TNRD_V6"; impl_->session.compression = "zstd";
    // A sentinel header: correct magic, version and CRC, but an empty index.
    // The file identifies itself as V6 from its first byte, while a reader
    // seeing metadataSize == 0 knows the index was never written and routes to
    // the recovery scan rather than rejecting the file.
    const auto sentinel = makeHeader(0, 0, 0, 0, 0);
    if (!writeAll(impl_->file,sentinel.data(),sentinel.size())) {
        fail(errorOut,"could not initialize V6 file"); std::fclose(impl_->file); impl_->file=nullptr; return false;
    }
    if (!impl_->writeSessionRecord(errorOut)) {
        std::fclose(impl_->file); impl_->file=nullptr; return false;
    }
    return true;
}
void TnrdV6Writer::setCompressionLevel(int level) {
    if (!impl_) return;
    impl_->compressionLevel = level < ZSTD_minCLevel() || level > ZSTD_maxCLevel()
        ? DEFAULT_COMPRESSION_LEVEL : level;
}
int TnrdV6Writer::compressionLevel() const { return impl_ ? impl_->compressionLevel : DEFAULT_COMPRESSION_LEVEL; }
bool TnrdV6Writer::appendRow(std::string_view row,float time,std::string* errorOut) {
    if (!isOpen()) { fail(errorOut,"V6 writer is not open"); return false; }
    return impl_->appendRow(row,time,errorOut);
}
bool TnrdV6Writer::append(const std::vector<V6SourceRow>& rows,std::string* errorOut) {
    for (const auto& row : rows) if (!appendRow(row.line,row.sessionTime,errorOut)) return false; return true;
}
bool TnrdV6Writer::appendViews(const std::vector<std::pair<std::string_view,float>>& rows,std::string* errorOut) {
    for (const auto& [row,time] : rows) if (!appendRow(row,time,errorOut)) return false; return true;
}
bool TnrdV6Writer::advanceSessionTime(float time,std::string* errorOut) {
    if (!isOpen()) { fail(errorOut,"V6 writer is not open"); return false; }
    if (std::isfinite(time)) impl_->phaseTime[impl_->phaseIndex(impl_->phase)] = std::max(impl_->now(),time);
    return impl_->commit(false,errorOut);
}
bool TnrdV6Writer::checkpoint(std::string* errorOut) {
    if (!isOpen()) return false;
    const uint64_t snapshotsBefore = impl_->checkpoints;
    if (!impl_->commit(false,errorOut)) return false;
    return impl_->checkpoints != snapshotsBefore || impl_->snapshot(errorOut);
}
bool TnrdV6Writer::rewind(float time,std::string* errorOut) {
    if (!isOpen()) { fail(errorOut,"V6 writer is not open"); return false; } return impl_->rewind(time,errorOut);
}
void TnrdV6Writer::abort() {
    if (isOpen()) { std::fclose(impl_->file); impl_->file = nullptr; }
}
bool TnrdV6Writer::finish(std::string* errorOut) {
    if (!isOpen()) return true;
    const float time = std::max(0.0f,impl_->now());
    for (uint8_t index=0;index<impl_->drivers.size();++index) if (impl_->drivers[index].open)
        impl_->closeLap(index,time,0,0,0,0,false,!impl_->drivers[index].currentInvalid,false);
    // One index, written once, directly after the last chunk. With no
    // superseded checkpoints there is nothing to reclaim, so the file is
    // already in its compacted layout and closing is the whole of finishing.
    const bool ok = impl_->commit(true,errorOut) && impl_->snapshot(errorOut);
    if (impl_->file) {
        const bool closed = std::fclose(impl_->file) == 0;
        impl_->file = nullptr;
        if (!closed && ok) { fail(errorOut,"could not close V6 file"); return false; }
    }
    return ok;
}
TnrdV6WriterMemoryStats TnrdV6Writer::memoryStats() const {
    TnrdV6WriterMemoryStats out; out.open=isOpen(); if(!impl_) return out;
    for(const auto& state:impl_->drivers){out.builderCount+=state.chunks.size();for(const auto&[_,b]:state.chunks){out.builderPlainBytes+=b.plain.size();out.builderPlainCapacityBytes+=b.plain.capacity();}out.pendingLapCount+=state.pending.size();for(const auto&lap:state.pending)for(const auto&[_,b]:lap.chunks)out.pendingLapPlainBytes+=b.plain.size();}
    out.chunkCount=impl_->chunks.size();out.lapCount=impl_->committedLaps.size();out.eventCount=impl_->committedShared.size();
    out.chunkWrites=impl_->chunkWrites;out.chunkPlainBytesProcessed=impl_->plainBytes;out.chunkCompressedBytesWritten=impl_->compressedBytes;out.compressionBufferBytesAllocated=impl_->compressionAllocated;out.compressionScratchCapacityBytes=impl_->scratch.capacity();out.compressionContextBytes=impl_->compressor?ZSTD_sizeof_CCtx(impl_->compressor):0;out.lastChunkPlainBytes=impl_->lastPlain;out.lastChunkCompressedBytes=impl_->lastCompressed;out.peakCompressionBufferCapacityBytes=impl_->peakScratch;out.checkpointWrites=impl_->checkpoints;out.retainedBytes=out.builderPlainCapacityBytes+out.pendingLapPlainBytes+out.compressionScratchCapacityBytes;return out;
}
bool writeTnrdV6(const std::string& path,const HeaderRow& header,const std::vector<V6SourceRow>& rows,std::string* errorOut){TnrdV6Writer writer;return writer.open(path,header,errorOut)&&writer.append(rows,errorOut)&&writer.finish(errorOut);}

// ARCHIVE_IMPLEMENTATION

struct TnrdV6Archive::Impl {
    struct Cached {
        std::shared_ptr<std::string> plain;
        std::list<size_t>::iterator lru;
    };

    std::FILE* file{};
    uint64_t fileSize{};
    bool recovered{};
    HeaderRow session;
    std::vector<V6DriverHeader> drivers;
    std::vector<V6LapSummary> lapSummaries;
    std::vector<V6ChunkInfo> v6Chunks;
    std::vector<V6SharedRecord> shared;
    std::vector<V4LapInfo> compatibleLaps;
    std::vector<V4ChunkInfo> compatibleChunks;
    V6ControlSummary control;
    std::map<uint32_t, size_t> lapById;
    std::map<uint8_t, size_t> driverByIndex;
    std::optional<uint8_t> player;
    uint8_t playback{};
    std::set<uint8_t> requestedTypes;
    float first{};
    float last{};

    mutable std::mutex cacheMutex;
    std::unordered_map<size_t, Cached> cache;
    std::list<size_t> lru;
    size_t cacheLimit{DEFAULT_CACHE_BYTES};
    size_t cacheUsed{};
    uint64_t decompressions{};

    float logical(V6Phase, float time) const { return time; }
    void clearCache() {
        std::lock_guard lock(cacheMutex);
        cache.clear(); lru.clear(); cacheUsed = 0;
    }
    void rebuildCompatibility() {
        // Match V5's active race branch: formation remains in the archive but
        // is not exposed through the application playback timeline.
        compatibleLaps.clear(); compatibleChunks.clear(); lapById.clear();
        std::set<uint32_t> lapsWithChunks;
        for (const auto& chunk : v6Chunks) lapsWithChunks.insert(chunk.lapId);
        for (size_t i = 0; i < lapSummaries.size(); ++i) {
            lapById[lapSummaries[i].lapId] = i;
        }
        for (const auto& lap : lapSummaries)
        if (lap.driverIndex == playback && lap.phase == V6Phase::Race) {
            if (!lapsWithChunks.contains(lap.lapId) && lap.startSessionTime == lap.endSessionTime)
                continue;
            V4LapInfo info;
            info.lapNumber = lap.lapNumber;
            info.startSessionTime = logical(lap.phase, lap.startSessionTime);
            info.endSessionTime = logical(lap.phase, lap.endSessionTime);
            info.lapTimeMs = lap.lapTimeMs;
            info.flags = (lap.isCompleted ? 1u : 0u) | (lap.isValid ? 2u : 0u) |
                         (lap.isPartial ? 4u : 0u);
            compatibleLaps.push_back(info);
        }
        std::sort(compatibleLaps.begin(), compatibleLaps.end(), [](const auto& a, const auto& b) {
            return std::tie(a.startSessionTime, a.lapNumber) < std::tie(b.startSessionTime, b.lapNumber);
        });
        for (const auto& chunk : v6Chunks) {
            if (chunk.phase != V6Phase::Race) continue;
            const auto lap = lapById.find(chunk.lapId);
            compatibleChunks.push_back({lap == lapById.end() ? 0u :
                lapSummaries[lap->second].lapNumber,
                chunk.typeId, chunk.flags, chunk.offset, chunk.compressedSize,
                chunk.uncompressedSize, chunk.sampleCount, chunk.checksum, chunk.sequence});
        }
        first = std::numeric_limits<float>::infinity(); last = 0.0f;
        for (const auto& lap : lapSummaries)
        if (lap.driverIndex == playback && lap.phase == V6Phase::Race) {
            first = std::min(first, logical(lap.phase, lap.startSessionTime));
            last = std::max(last, logical(lap.phase, lap.endSessionTime));
        }
        if (!std::isfinite(first)) first = 0.0f;
        control.startSessionTime = first; control.totalSessionTime = std::max(first, last);
        control.events.clear();
        for (const auto& record : shared)
            if (record.phase == V6Phase::Race && sharedRowType(record.json) == 6)
                control.events.push_back(record.json);
    }
};

namespace {

std::string withDriver(std::string_view json, uint8_t driver);

bool parseChunkRows(std::string_view plain, const V6ChunkInfo& chunk, float logicalOffset,
                    std::vector<V6TimedRow>& out, float from, float to,
                    const IndexedCancelCheck& cancelled = {}) {
    size_t start = 0; uint32_t source = 0;
    while (start < plain.size()) {
        if (cancelled && cancelled()) return false;
        const size_t end = plain.find('\n', start);
        const size_t length = (end == std::string_view::npos ? plain.size() : end) - start;
        if (length) {
            const auto line = plain.substr(start, length);
            const float raw = scanTime(line); const float time = raw + logicalOffset;
            if (time >= from && time <= to)
                out.push_back({time, chunk.typeId, chunk.sequence,
                               withDriver(line, chunk.driverIndex), source});
            ++source;
        }
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return true;
}

std::string withDriver(std::string_view json, uint8_t driver) {
    if (json.empty() || json.front() != '{') return std::string(json);
    std::string out; out.reserve(json.size() + 20);
    out += "{\"driver_idx\":" + std::to_string(driver);
    if (json.size() > 1) { out.push_back(','); out.append(json.substr(1)); }
    else out.push_back('}');
    return out;
}

} // namespace

TnrdV6Archive::TnrdV6Archive() : impl_(std::make_unique<Impl>()) {}
TnrdV6Archive::~TnrdV6Archive() { close(); }

namespace {

// One record as the recovery scan sees it: a 32-byte prefix at `at`, followed
// by `compressedSize` payload bytes.
struct ScannedRecord {
    uint32_t magic{};
    uint64_t payloadOffset{};
    uint64_t compressedSize{};
    uint64_t uncompressedSize{};
    uint8_t driverIndex{};
    uint8_t typeId{};
    uint8_t flags{};
    V6Phase phase{V6Phase::Race};
    uint32_t lapId{};
    uint32_t sampleCount{};
};

// Reads one flat sample field, e.g. scanField(line, "lap_num"). Samples written
// by the V6 writer are flat single-line objects, so a key search is enough and
// no JSON parse is needed.
bool scanField(std::string_view json, std::string_view key, double& valueOut) {
    std::string needle;
    needle.reserve(key.size() + 3);
    needle.push_back('"');
    needle.append(key);
    needle.append("\":");
    const auto at = json.find(needle);
    if (at == std::string_view::npos) return false;
    const auto start = at + needle.size();
    if (start >= json.size()) return false;
    if (json[start] == 't' || json[start] == 'f') {
        valueOut = json[start] == 't' ? 1.0 : 0.0;
        return true;
    }
    char* parsedEnd = nullptr;
    const double value = std::strtod(json.data() + start, &parsedEnd);
    if (parsedEnd == json.data() + start) return false;
    valueOut = value;
    return true;
}

double fieldOr(std::string_view json, std::string_view key, double fallback) {
    double value = fallback;
    return scanField(json, key, value) ? value : fallback;
}

std::string_view firstLine(std::string_view plain) {
    const auto end = plain.find('\n');
    return end == std::string_view::npos ? plain : plain.substr(0, end);
}

std::string_view lastLine(std::string_view plain) {
    auto end = plain.size();
    while (end && plain[end - 1] == '\n') --end;
    if (!end) return {};
    const auto start = plain.rfind('\n', end - 1);
    return plain.substr(start == std::string_view::npos ? 0 : start + 1,
                        start == std::string_view::npos ? end : end - start - 1);
}

} // namespace

bool TnrdV6Archive::open(const std::string& path, HeaderRow& headerOut, std::string* errorOut) {
    close(); impl_ = std::make_unique<Impl>();
    impl_->file = openTnrdFile(path, "rb");
    if (!impl_->file) { fail(errorOut, "could not open V6 file"); return false; }
    if (!seekEnd(impl_->file) || (impl_->fileSize = tellFile(impl_->file)) < HEADER_SIZE) {
        fail(errorOut, "truncated V6 file"); close(); return false;
    }
    if (impl_->fileSize < HEADER_SIZE + FOOTER_SIZE) return recoverByScan(headerOut, errorOut);
    std::array<uint8_t, HEADER_SIZE> header{};
    if (!readAt(impl_->file, 0, header.data(), header.size()) ||
        !std::equal(MAGIC.begin(), MAGIC.end(), header.begin()) || get16(header.data() + 8) != 6 ||
        get16(header.data() + 10) != HEADER_SIZE || get32(header.data() + 12) != 3 ||
        get32(header.data() + 44) != CHUNK_ENTRY_SIZE ||
        get32(header.data() + 120) != static_cast<uint32_t>(::crc32(0, header.data(), 120))) {
        fail(errorOut, "invalid V6 header"); close(); return false;
    }
    const uint64_t metadataOffset = get64(header.data() + 16);
    const uint64_t metadataSize = get64(header.data() + 24);
    const uint64_t directoryOffset = get64(header.data() + 32);
    const uint32_t chunkCount = get32(header.data() + 40);
    const uint64_t footerOffset = get64(header.data() + 48);
    if (!metadataSize || metadataSize > MAX_METADATA_BYTES || chunkCount > MAX_CHUNKS ||
        !rangeOk(impl_->fileSize, metadataOffset, metadataSize) ||
        !rangeOk(impl_->fileSize, directoryOffset, uint64_t(chunkCount) * CHUNK_ENTRY_SIZE) ||
        !rangeOk(impl_->fileSize, footerOffset, FOOTER_SIZE) ||
        metadataOffset + metadataSize != directoryOffset ||
        directoryOffset + uint64_t(chunkCount) * CHUNK_ENTRY_SIZE != footerOffset) {
        return recoverByScan(headerOut, errorOut);
    }
    std::array<uint8_t, FOOTER_SIZE> footer{};
    if (!readAt(impl_->file, footerOffset, footer.data(), footer.size()) ||
        get32(footer.data()) != FOOTER_MAGIC || get16(footer.data() + 4) != 6 ||
        get16(footer.data() + 6) != FOOTER_SIZE || get64(footer.data() + 8) != metadataOffset ||
        get64(footer.data() + 16) != directoryOffset || get32(footer.data() + 24) != chunkCount ||
        get32(footer.data() + 28) != metadataSize) {
        return recoverByScan(headerOut, errorOut);
    }
    std::string metadata(metadataSize, '\0');
    std::vector<uint8_t> directory(uint64_t(chunkCount) * CHUNK_ENTRY_SIZE);
    if (!readAt(impl_->file, metadataOffset, metadata.data(), metadata.size()) ||
        !readAt(impl_->file, directoryOffset, directory.data(), directory.size()) ||
        get32(footer.data() + 32) != controlCrc(metadata, directory)) {
        return recoverByScan(headerOut, errorOut);
    }
    V6Metadata decoded;
    if (const auto ec = glz::read_json(decoded, metadata); ec) {
        return recoverByScan(headerOut, errorOut);
    }
    impl_->session = std::move(decoded.session); impl_->drivers = std::move(decoded.drivers);
    impl_->lapSummaries = std::move(decoded.laps);
    std::set<uint8_t> driverIds; std::set<uint32_t> lapIds;
    std::map<uint32_t, std::pair<uint8_t, V6Phase>> lapOwners;
    for (size_t i = 0; i < impl_->drivers.size(); ++i) {
        const auto id = impl_->drivers[i].vehicleIndex;
        if (id >= 24 || !driverIds.insert(id).second) { fail(errorOut, "invalid V6 driver table"); close(); return false; }
        impl_->driverByIndex[id] = i;
        if (impl_->drivers[i].isPlayer) impl_->player = id;
    }
    for (const auto& lap : impl_->lapSummaries) {
        if (lap.driverIndex >= 24 || lap.lapId == 0 || !lapIds.insert(lap.lapId).second ||
            !std::isfinite(lap.startSessionTime) || !std::isfinite(lap.endSessionTime) ||
            lap.endSessionTime < lap.startSessionTime) {
            fail(errorOut, "invalid V6 lap table"); close(); return false;
        }
        lapOwners[lap.lapId] = {lap.driverIndex, lap.phase};
    }
    impl_->v6Chunks.reserve(chunkCount);
    uint64_t priorPayloadEnd = HEADER_SIZE;
    for (uint32_t i = 0; i < chunkCount; ++i) {
        const uint8_t* p = directory.data() + uint64_t(i) * CHUNK_ENTRY_SIZE;
        V6ChunkInfo chunk{p[0], get32(p + 4), p[1], p[2], static_cast<V6Phase>(p[3]),
            getFloat(p + 8), getFloat(p + 12), get64(p + 16), get64(p + 24), get64(p + 32),
            get32(p + 40), get32(p + 44), get64(p + 48)};
        const auto owner = lapOwners.find(chunk.lapId);
        if (chunk.driverIndex >= 24 || chunk.typeId == 0 || chunk.typeId >= static_cast<uint8_t>(V6DataType::Count) ||
            chunk.phase > V6Phase::Formation || !lapIds.contains(chunk.lapId) ||
            owner == lapOwners.end() || owner->second.first != chunk.driverIndex || owner->second.second != chunk.phase ||
            !std::isfinite(chunk.firstTime) || !std::isfinite(chunk.lastTime) || chunk.lastTime < chunk.firstTime ||
            !chunk.compressedSize || !chunk.uncompressedSize || chunk.uncompressedSize > MAX_CHUNK_PLAIN ||
            chunk.offset < CHUNK_PREFIX_SIZE || !rangeOk(impl_->fileSize, chunk.offset, chunk.compressedSize) ||
            chunk.offset + chunk.compressedSize > metadataOffset || chunk.offset - CHUNK_PREFIX_SIZE < priorPayloadEnd) {
            fail(errorOut, "invalid V6 chunk directory"); close(); return false;
        }
        std::array<uint8_t, CHUNK_PREFIX_SIZE> prefix{};
        if (!readAt(impl_->file, chunk.offset - CHUNK_PREFIX_SIZE, prefix.data(), prefix.size()) ||
            get32(prefix.data()) != CHUNK_MAGIC || prefix[4] != chunk.driverIndex || prefix[5] != chunk.typeId ||
            prefix[7] != static_cast<uint8_t>(chunk.phase) || get32(prefix.data() + 8) != chunk.lapId ||
            get64(prefix.data() + 12) != chunk.compressedSize || get64(prefix.data() + 20) != chunk.uncompressedSize ||
            get32(prefix.data() + 28) != chunk.sampleCount) {
            fail(errorOut, "invalid V6 chunk prefix"); close(); return false;
        }
        priorPayloadEnd = chunk.offset + chunk.compressedSize;
        impl_->v6Chunks.push_back(chunk);
    }
    impl_->shared.reserve(decoded.shared.size());
    for (const auto& stored : decoded.shared) {
        if (stored.phase > V6Phase::Formation || !std::isfinite(stored.sessionTime) ||
            !stored.compressedSize || !stored.uncompressedSize || stored.uncompressedSize > MAX_CHUNK_PLAIN ||
            stored.offset < CHUNK_PREFIX_SIZE || !rangeOk(impl_->fileSize, stored.offset, stored.compressedSize) ||
            stored.offset + stored.compressedSize > metadataOffset) {
            fail(errorOut, "invalid V6 shared-record index"); close(); return false;
        }
        std::array<uint8_t, CHUNK_PREFIX_SIZE> prefix{};
        if (!readAt(impl_->file, stored.offset - CHUNK_PREFIX_SIZE, prefix.data(), prefix.size()) ||
            get32(prefix.data()) != SHARED_MAGIC || prefix[4] != static_cast<uint8_t>(stored.phase) ||
            get64(prefix.data() + 8) != stored.compressedSize ||
            get64(prefix.data() + 16) != stored.uncompressedSize || get32(prefix.data() + 24) != stored.checksum) {
            fail(errorOut, "invalid V6 shared-record prefix"); close(); return false;
        }
        std::vector<uint8_t> compressed(stored.compressedSize);
        std::string plain(stored.uncompressedSize, '\0');
        if (!readAt(impl_->file, stored.offset, compressed.data(), compressed.size())) {
            fail(errorOut, "could not read V6 shared record"); close(); return false;
        }
        const size_t size = ZSTD_decompress(plain.data(), plain.size(), compressed.data(), compressed.size());
        if (ZSTD_isError(size) || size != stored.uncompressedSize ||
            static_cast<uint32_t>(::crc32(0, reinterpret_cast<const Bytef*>(plain.data()),
                                          static_cast<uInt>(plain.size()))) != stored.checksum) {
            fail(errorOut, "invalid V6 shared-record payload"); close(); return false;
        }
        impl_->shared.push_back({stored.phase, stored.sessionTime, std::move(plain)});
    }
    impl_->playback = impl_->player.value_or(impl_->drivers.empty() ? 0 : impl_->drivers.front().vehicleIndex);
    impl_->rebuildCompatibility(); headerOut = impl_->session; return true;
}

bool TnrdV6Archive::wasRecovered() const { return impl_ && impl_->recovered; }

// Rebuilds a recording whose index is missing, stale or corrupt by walking the
// append-only record stream. Everything the index held is derivable: chunk
// entries from their prefixes, lap summaries by replaying LapTiming samples the
// way the writer's boundary() derives them, driver headers from the chunks that
// exist plus the participants rows in the shared records, and the session
// header from the SES6 record written at open(). The scan stops at the first
// record that does not validate, which is how a torn tail costs the partial
// chunk instead of the recording. It never writes to the file: a live recorder
// may still hold it open.
bool TnrdV6Archive::recoverByScan(HeaderRow& headerOut, std::string* errorOut) {
    impl_->drivers.clear(); impl_->lapSummaries.clear();
    impl_->v6Chunks.clear(); impl_->shared.clear();
    impl_->driverByIndex.clear(); impl_->player.reset();

    std::map<uint8_t, V6DriverHeader> headers;
    struct LapAccum {
        V6LapSummary summary;
        bool haveBounds{};
        bool haveTiming{};
        uint32_t nextLapTimeMs{}, nextS1Ms{}, nextS2Ms{};
        uint32_t firstLapNumber{};
        bool lastInvalid{};
    };
    std::map<uint32_t, LapAccum> laps;
    std::map<uint8_t, std::vector<uint32_t>> driverLapOrder;
    std::vector<uint8_t> compressed;
    std::string plain;
    uint64_t at = HEADER_SIZE;
    uint64_t sequence = 1;
    bool sawSession = false;

    while (at + CHUNK_PREFIX_SIZE <= impl_->fileSize) {
        std::array<uint8_t, CHUNK_PREFIX_SIZE> prefix{};
        if (!readAt(impl_->file, at, prefix.data(), prefix.size())) break;
        const uint32_t magic = get32(prefix.data());
        if (magic != CHUNK_MAGIC && magic != SHARED_MAGIC && magic != SESSION_MAGIC) break;

        ScannedRecord record;
        record.magic = magic;
        record.payloadOffset = at + CHUNK_PREFIX_SIZE;
        if (magic == CHUNK_MAGIC) {
            record.driverIndex = prefix[4]; record.typeId = prefix[5]; record.flags = prefix[6];
            record.phase = static_cast<V6Phase>(prefix[7]);
            record.lapId = get32(prefix.data() + 8);
            record.compressedSize = get64(prefix.data() + 12);
            record.uncompressedSize = get64(prefix.data() + 20);
            record.sampleCount = get32(prefix.data() + 28);
            if (record.driverIndex >= 24 || record.typeId == 0 ||
                record.typeId >= static_cast<uint8_t>(V6DataType::Count) ||
                record.phase > V6Phase::Formation || record.lapId == 0) break;
        } else {
            record.phase = static_cast<V6Phase>(prefix[4]);
            record.compressedSize = get64(prefix.data() + 8);
            record.uncompressedSize = get64(prefix.data() + 16);
            if (magic == SHARED_MAGIC && record.phase > V6Phase::Formation) break;
        }
        // A truncated tail: the declared payload runs past end of file.
        if (!record.compressedSize || !record.uncompressedSize ||
            record.uncompressedSize > MAX_CHUNK_PLAIN ||
            !rangeOk(impl_->fileSize, record.payloadOffset, record.compressedSize)) break;

        compressed.resize(static_cast<size_t>(record.compressedSize));
        if (!readAt(impl_->file, record.payloadOffset, compressed.data(), compressed.size())) break;
        if (magic == SESSION_MAGIC) {
            if (record.compressedSize != record.uncompressedSize) break;
            plain.assign(reinterpret_cast<const char*>(compressed.data()), compressed.size());
        } else {
            // zstd frames carry a checksum (writeChunk sets checksumFlag), so a
            // corrupt but correctly sized payload fails here rather than
            // silently decoding to garbage.
            plain.assign(static_cast<size_t>(record.uncompressedSize), char{0});
            const size_t size = ZSTD_decompress(plain.data(), plain.size(),
                                                compressed.data(), compressed.size());
            if (ZSTD_isError(size) || size != record.uncompressedSize) break;
        }
        at = record.payloadOffset + record.compressedSize;

        if (magic == SESSION_MAGIC) {
            HeaderRow decoded;
            if (!glz::read_json(decoded, plain)) { impl_->session = std::move(decoded); sawSession = true; }
            continue;
        }
        if (magic == SHARED_MAGIC) {
            const float time = scanTime(firstLine(plain));
            const float stamp = std::isfinite(time) ? time : 0.0f;
            impl_->shared.push_back({record.phase, stamp, plain});
            if (rowType(plain) == "participants") {
                ParticipantsRow row;
                if (!glz::read<kPartialRead>(row, plain)) {
                    if (row.player_idx >= 0 && row.player_idx < 24)
                        impl_->player = static_cast<uint8_t>(row.player_idx);
                    for (const auto& driver : row.drivers) {
                        if (driver.idx < 0 || driver.idx >= 24) continue;
                        auto& header = headers[static_cast<uint8_t>(driver.idx)];
                        header.vehicleIndex = static_cast<uint8_t>(driver.idx);
                        if (header.driverName.empty()) {
                            header.driverName = driver.name;
                            header.teamId = driver.team_id;
                            header.raceNumber = driver.race_number;
                        }
                        header.isPlayer = driver.idx == row.player_idx;
                        const auto setting = !driver.your_telemetry ? TelemetrySetting::Unknown
                            : (*driver.your_telemetry == 1 ? TelemetrySetting::Public
                                                           : TelemetrySetting::Restricted);
                        if (setting == TelemetrySetting::Unknown) continue;
                        if (header.initialTelemetrySetting == TelemetrySetting::Unknown) {
                            header.initialTelemetrySetting = setting;
                        } else {
                            const auto previous = header.restrictionChanges.empty()
                                ? header.initialTelemetrySetting
                                : header.restrictionChanges.back().setting;
                            if (previous != setting)
                                header.restrictionChanges.push_back({record.phase, stamp, setting});
                        }
                    }
                }
            }
            continue;
        }

        // A chunk. Its directory entry is the prefix plus the sample time range
        // and payload checksum, which only the payload carries.
        float first = std::numeric_limits<float>::max();
        float last = std::numeric_limits<float>::lowest();
        for (size_t start = 0; start < plain.size();) {
            auto end = plain.find(char{10}, start);
            if (end == std::string::npos) end = plain.size();
            if (end > start) {
                const float time = scanTime(std::string_view(plain.data() + start, end - start));
                if (std::isfinite(time)) { first = std::min(first, time); last = std::max(last, time); }
            }
            start = end + 1;
        }
        if (first > last) { first = 0.0f; last = 0.0f; }

        V6ChunkInfo chunk{record.driverIndex, record.lapId, record.typeId, record.flags,
                          record.phase, first, last, record.payloadOffset,
                          record.compressedSize, record.uncompressedSize, record.sampleCount,
                          static_cast<uint32_t>(::crc32(0, reinterpret_cast<const Bytef*>(plain.data()),
                                                        static_cast<uInt>(plain.size()))),
                          sequence++};
        impl_->v6Chunks.push_back(chunk);

        auto& header = headers[record.driverIndex];
        header.vehicleIndex = record.driverIndex;
        header.availableTypeMask |= v6DataTypeBit(static_cast<V6DataType>(record.typeId));

        auto [lapIt, inserted] = laps.try_emplace(record.lapId);
        auto& accum = lapIt->second;
        if (inserted) {
            accum.summary.lapId = record.lapId;
            accum.summary.driverIndex = record.driverIndex;
            accum.summary.phase = record.phase;
            driverLapOrder[record.driverIndex].push_back(record.lapId);
            header.lapIds.push_back(record.lapId);
        }
        if (!accum.haveBounds) {
            accum.summary.startSessionTime = first; accum.summary.endSessionTime = last;
            accum.haveBounds = true;
        } else {
            accum.summary.startSessionTime = std::min(accum.summary.startSessionTime, first);
            accum.summary.endSessionTime = std::max(accum.summary.endSessionTime, last);
        }
        if (static_cast<V6DataType>(record.typeId) == V6DataType::LapTiming && !plain.empty()) {
            const auto head = firstLine(plain);
            const auto tail = lastLine(plain);
            accum.haveTiming = true;
            accum.firstLapNumber = static_cast<uint32_t>(std::max(0.0, fieldOr(head, "lap_num", 0.0)));
            accum.nextLapTimeMs = static_cast<uint32_t>(std::max(0.0, fieldOr(head, "last_lap_ms", 0.0)));
            accum.nextS1Ms = static_cast<uint32_t>(std::max(0.0, fieldOr(head, "s1_ms", 0.0)));
            accum.nextS2Ms = static_cast<uint32_t>(std::max(0.0, fieldOr(head, "s2_ms", 0.0)));
            accum.lastInvalid = fieldOr(tail.empty() ? head : tail, "lap_invalid", 0.0) != 0.0;
        }
    }

    if (impl_->v6Chunks.empty()) {
        fail(errorOut, sawSession ? "V6 recording contains no recoverable data"
                                  : "invalid V6 file: no index and no recoverable records");
        close(); return false;
    }

    // The game reports a lap time one lap late, so a lap's time and sectors come
    // from the first sample of that driver's next lap. This mirrors boundary()
    // in the writer; the two must stay in step.
    for (const auto& entry : driverLapOrder) {
        const auto& order = entry.second;
        for (size_t i = 0; i < order.size(); ++i) {
            auto& accum = laps[order[i]];
            accum.summary.lapNumber = accum.firstLapNumber;
            accum.summary.isValid = !accum.lastInvalid;
            if (i + 1 < order.size()) {
                const auto& next = laps[order[i + 1]];
                if (next.haveTiming) {
                    const uint32_t total = next.nextLapTimeMs;
                    accum.summary.lapTimeMs = total;
                    accum.summary.s1Ms = next.nextS1Ms;
                    accum.summary.s2Ms = next.nextS2Ms;
                    accum.summary.s3Ms = total > next.nextS1Ms + next.nextS2Ms
                        ? total - next.nextS1Ms - next.nextS2Ms : 0;
                    accum.summary.isCompleted =
                        next.firstLapNumber > accum.summary.lapNumber && total > 0;
                }
            }
            accum.summary.isPartial = !accum.summary.isCompleted;
        }
    }
    impl_->lapSummaries.reserve(laps.size());
    for (auto& entry : laps) impl_->lapSummaries.push_back(entry.second.summary);

    impl_->drivers.reserve(headers.size());
    for (auto& entry : headers) impl_->drivers.push_back(std::move(entry.second));
    for (size_t i = 0; i < impl_->drivers.size(); ++i) {
        const auto id = impl_->drivers[i].vehicleIndex;
        if (id >= 24) { fail(errorOut, "invalid V6 driver table after recovery"); close(); return false; }
        impl_->driverByIndex[id] = i;
        if (impl_->drivers[i].isPlayer) impl_->player = id;
    }
    impl_->playback = impl_->player.value_or(
        impl_->drivers.empty() ? 0 : impl_->drivers.front().vehicleIndex);
    impl_->session.magic = "TNRD_V6";
    impl_->session.compression = "zstd";
    impl_->recovered = true;
    impl_->rebuildCompatibility();
    headerOut = impl_->session;
    if (errorOut) errorOut->clear();
    return true;
}

void TnrdV6Archive::close() {
    if (impl_ && impl_->file) std::fclose(impl_->file);
    if (impl_) { impl_->file = nullptr; impl_->clearCache(); }
}
bool TnrdV6Archive::isOpen() const { return impl_ && impl_->file; }
const std::vector<V6LapInfo>& TnrdV6Archive::laps() const { return impl_->compatibleLaps; }
const std::vector<V4ChunkInfo>& TnrdV6Archive::chunks() const { return impl_->compatibleChunks; }
const V6ControlSummary& TnrdV6Archive::summary() const { return impl_->control; }
float TnrdV6Archive::startTime() const { return impl_->first; }
float TnrdV6Archive::totalTime() const { return std::max(impl_->first, impl_->last); }
int TnrdV6Archive::lapAt(float time) const {
    int result = -1; for (const auto& lap : impl_->compatibleLaps)
        if (time >= lap.startSessionTime && time <= lap.endSessionTime) result = static_cast<int>(lap.lapNumber);
    return result;
}
void TnrdV6Archive::chunkIndicesForLap(uint32_t lap, V6RowTypeMask mask, std::vector<size_t>& out) const {
    out.clear();
    for (size_t i = 0; i < impl_->v6Chunks.size(); ++i) {
        const auto& chunk = impl_->v6Chunks[i]; const auto found = impl_->lapById.find(chunk.lapId);
        if ((impl_->requestedTypes.empty() || impl_->requestedTypes.contains(chunk.typeId)) &&
            chunk.driverIndex == impl_->playback && found != impl_->lapById.end() &&
            impl_->lapSummaries[found->second].phase == V6Phase::Race &&
            impl_->lapSummaries[found->second].lapNumber == lap &&
            requestedByOldMask(static_cast<V6DataType>(chunk.typeId), mask)) out.push_back(i);
    }
}
bool TnrdV6Archive::chunkTimeBounds(size_t index, float& firstOut, float& lastOut) const {
    if (index >= impl_->v6Chunks.size()) return false; const auto& chunk = impl_->v6Chunks[index];
    firstOut = chunk.firstTime; lastOut = chunk.lastTime; return true;
}
void TnrdV6Archive::prefetchChunk(size_t index) { std::shared_ptr<std::string> ignored; (void)loadChunkPlain(index, ignored, nullptr); }
void TnrdV6Archive::cancelPrefetch() {}

bool TnrdV6Archive::loadChunkPlain(size_t index, std::shared_ptr<std::string>& out, std::string* errorOut) {
    if (!isOpen() || index >= impl_->v6Chunks.size()) { fail(errorOut, "invalid V6 chunk index"); return false; }
    {
        std::lock_guard lock(impl_->cacheMutex); const auto found = impl_->cache.find(index);
        if (found != impl_->cache.end()) {
            impl_->lru.splice(impl_->lru.begin(), impl_->lru, found->second.lru);
            out = found->second.plain; return true;
        }
    }
    const auto& chunk = impl_->v6Chunks[index]; std::vector<uint8_t> compressed(chunk.compressedSize);
    if (!readAt(impl_->file, chunk.offset, compressed.data(), compressed.size())) { fail(errorOut, "could not read V6 chunk"); return false; }
    auto plain = std::make_shared<std::string>(chunk.uncompressedSize, '\0');
    const size_t size = ZSTD_decompress(plain->data(), plain->size(), compressed.data(), compressed.size());
    if (ZSTD_isError(size) || size != chunk.uncompressedSize ||
        static_cast<uint32_t>(::crc32(0, reinterpret_cast<const Bytef*>(plain->data()), static_cast<uInt>(plain->size()))) != chunk.checksum) {
        fail(errorOut, "invalid V6 chunk payload"); return false;
    }
    {
        std::lock_guard lock(impl_->cacheMutex); ++impl_->decompressions;
        if (plain->size() <= impl_->cacheLimit) {
            while (!impl_->lru.empty() && impl_->cacheUsed + plain->size() > impl_->cacheLimit) {
                const size_t victim = impl_->lru.back(); impl_->lru.pop_back();
                impl_->cacheUsed -= impl_->cache[victim].plain->size(); impl_->cache.erase(victim);
            }
            impl_->lru.push_front(index); impl_->cache[index] = {plain, impl_->lru.begin()}; impl_->cacheUsed += plain->size();
        }
    }
    out = std::move(plain); return true;
}

bool TnrdV6Archive::rowsForChunks(const std::vector<size_t>& indices, std::vector<std::vector<V6TimedRow>>& out,
                                  std::string* errorOut) {
    out.clear(); out.reserve(indices.size());
    for (size_t index : indices) {
        if (index >= impl_->v6Chunks.size()) { fail(errorOut, "invalid V6 chunk index"); return false; }
        std::shared_ptr<std::string> plain; if (!loadChunkPlain(index, plain, errorOut)) return false;
        out.emplace_back(); const auto& chunk = impl_->v6Chunks[index];
        if (!parseChunkRows(*plain, chunk, 0.0f,
                            out.back(), -std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity())) return false;
    }
    return true;
}
bool TnrdV6Archive::rowsForLap(uint32_t lap, V6RowTypeMask mask, std::vector<V6TimedRow>& out, std::string* errorOut) {
    return rowsForLapRange(lap, -std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity(), mask, out, errorOut);
}
bool TnrdV6Archive::rowsForLapRange(uint32_t lap, float from, float to, V6RowTypeMask mask,
                                    std::vector<V6TimedRow>& out, std::string* errorOut,
                                    const IndexedCancelCheck& cancelled) {
    out.clear(); std::vector<size_t> indices; chunkIndicesForLap(lap, mask, indices);
    for (size_t index : indices) {
        std::shared_ptr<std::string> plain; if (!loadChunkPlain(index, plain, errorOut)) return false;
        const auto& chunk = impl_->v6Chunks[index];
        if (!parseChunkRows(*plain, chunk, 0.0f,
                            out, from, to, cancelled)) return false;
    }
    std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        return std::tie(a.sessionTime, a.sequence, a.sourceOffset) < std::tie(b.sessionTime, b.sequence, b.sourceOffset);
    }); return true;
}
bool TnrdV6Archive::rowsForRange(float from, float to, V6RowTypeMask mask, std::vector<V6TimedRow>& out,
                                 std::string* errorOut, const IndexedCancelCheck& cancelled) {
    out.clear();
    for (size_t index = 0; index < impl_->v6Chunks.size(); ++index) {
        const auto& chunk = impl_->v6Chunks[index];
        if (chunk.phase != V6Phase::Race) continue;
        const auto type = static_cast<V6DataType>(chunk.typeId);
        if (!impl_->requestedTypes.empty() && !impl_->requestedTypes.contains(chunk.typeId)) continue;
        if ((!requestedByOldMask(type, mask) || chunk.driverIndex != impl_->playback) &&
            !requestedForAllDrivers(type, mask)) continue;
        // V6's explicit type subscription is authoritative inside the legacy
        // family envelope: a telemetry history request for speed must not also
        // decompress RPM, controls, temperatures, and engine state.
        constexpr float offset = 0.0f;
        if (chunk.lastTime + offset < from || chunk.firstTime + offset > to) continue;
        std::shared_ptr<std::string> plain; if (!loadChunkPlain(index, plain, errorOut)) return false;
        if (!parseChunkRows(*plain, chunk, offset, out, from, to, cancelled)) return false;
    }
    for (size_t i = 0; i < impl_->shared.size(); ++i) {
        const auto& record = impl_->shared[i]; const uint8_t type = sharedRowType(record.json);
        if (record.phase != V6Phase::Race || !type || !(mask & v4TypeBit(type))) continue;
        const float time = impl_->logical(record.phase, record.sessionTime);
        if (time >= from && time <= to) out.push_back({time, type, i, record.json, 0});
    }
    std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        return std::tie(a.sessionTime, a.sequence, a.sourceOffset) < std::tie(b.sessionTime, b.sequence, b.sourceOffset);
    }); return true;
}
bool TnrdV6Archive::forEachRowInRange(float from, float to, V6RowTypeMask mask,
                                      const std::function<bool(const V6TimedRow&)>& callback,
                                      std::string* errorOut, const IndexedCancelCheck& cancelled) {
    std::vector<V6TimedRow> rows; if (!rowsForRange(from, to, mask, rows, errorOut, cancelled)) return false;
    for (const auto& row : rows) if ((cancelled && cancelled()) || !callback(row)) return false; return true;
}
bool TnrdV6Archive::latestRows(float at, const std::vector<uint8_t>& types, std::vector<V6TimedRow>& out,
                               std::string* errorOut, const IndexedCancelCheck& cancelled) {
    return readMultiDriverLatest(at, {impl_->playback}, types, out, errorOut);
}
bool TnrdV6Archive::forEachChunk(V6RowTypeMask mask,
                                 const std::function<bool(const V4ChunkInfo&, std::string_view)>& callback,
                                 std::string* errorOut) {
    for (size_t i = 0; i < impl_->v6Chunks.size(); ++i) {
        const auto& chunk = impl_->v6Chunks[i];
        if (chunk.phase != V6Phase::Race || chunk.driverIndex != impl_->playback ||
            !requestedByOldMask(static_cast<V6DataType>(chunk.typeId), mask)) continue;
        std::shared_ptr<std::string> plain; if (!loadChunkPlain(i, plain, errorOut)) return false;
        const auto lap = impl_->lapById.find(chunk.lapId);
        V4ChunkInfo info{lap == impl_->lapById.end() ? 0u : impl_->lapSummaries[lap->second].lapNumber,
            chunk.typeId, chunk.flags, chunk.offset, chunk.compressedSize, chunk.uncompressedSize,
            chunk.sampleCount, chunk.checksum, chunk.sequence};
        if (!callback(info, *plain)) return false;
    }
    return true;
}
void TnrdV6Archive::setCacheLimitBytes(size_t bytes) { impl_->cacheLimit = bytes; impl_->clearCache(); }
size_t TnrdV6Archive::cacheBytes() const { std::lock_guard lock(impl_->cacheMutex); return impl_->cacheUsed; }
uint64_t TnrdV6Archive::decompressedChunkCount() const { std::lock_guard lock(impl_->cacheMutex); return impl_->decompressions; }
size_t TnrdV6Archive::peakConcurrentChunkLoads() const { return isOpen() ? 1 : 0; }

void TnrdV6Archive::setPlaybackDriver(uint8_t index) {
    if (impl_->driverByIndex.contains(index)) { impl_->playback = index; impl_->rebuildCompatibility(); }
}
void TnrdV6Archive::setRequestedTypes(const std::vector<uint8_t>& types) {
    impl_->requestedTypes.clear();
    for (uint8_t type : types) if (type > 0 && type < static_cast<uint8_t>(V6DataType::Count))
        impl_->requestedTypes.insert(type);
}
bool TnrdV6Archive::requestedType(uint8_t type) const {
    return impl_->requestedTypes.empty() || impl_->requestedTypes.contains(type);
}
void TnrdV6Archive::playbackChunkIndices(V6RowTypeMask mask, std::vector<size_t>& out) const {
    out.clear();
    for (size_t i = 0; i < impl_->v6Chunks.size(); ++i) {
        const auto& chunk = impl_->v6Chunks[i];
        if (chunk.phase != V6Phase::Race) continue;
        const auto type = static_cast<V6DataType>(chunk.typeId);
        if (!impl_->requestedTypes.empty() && !impl_->requestedTypes.contains(chunk.typeId)) continue;
        if ((chunk.driverIndex == impl_->playback && requestedByOldMask(type, mask)) ||
            requestedForAllDrivers(type, mask)) out.push_back(i);
    }
    std::stable_sort(out.begin(), out.end(), [&](size_t left, size_t right) {
        const auto& a = impl_->v6Chunks[left]; const auto& b = impl_->v6Chunks[right];
        return std::tuple{impl_->logical(a.phase, a.firstTime), a.sequence} <
               std::tuple{impl_->logical(b.phase, b.firstTime), b.sequence};
    });
}
uint8_t TnrdV6Archive::playbackDriver() const { return impl_->playback; }
std::optional<uint8_t> TnrdV6Archive::playerDriverIndex() const { return impl_->player; }
const std::vector<V6DriverHeader>& TnrdV6Archive::driverHeaders() const { return impl_->drivers; }
const V6DriverHeader* TnrdV6Archive::driverHeader(uint8_t index) const {
    const auto found = impl_->driverByIndex.find(index); return found == impl_->driverByIndex.end() ? nullptr : &impl_->drivers[found->second];
}
TelemetrySetting TnrdV6Archive::telemetrySettingAt(uint8_t index, float logical) const {
    const auto* header = driverHeader(index);
    if (!header) return TelemetrySetting::Unknown;
    // restrictionChanges is appended in recording order, so it is already
    // sorted by (phase, session time); stop at the first change in the future.
    auto setting = header->initialTelemetrySetting;
    for (const auto& change : header->restrictionChanges) {
        if (impl_->logical(change.phase, change.sessionTime) > logical) break;
        setting = change.setting;
    }
    return setting;
}
bool TnrdV6Archive::privateDataAvailableAt(uint8_t index, float logical) const {
    // Mirrors the writer's privateAvailable(): a restricted recording player
    // still has their own private telemetry stored.
    return (impl_->player && *impl_->player == index) ||
        telemetrySettingAt(index, logical) == TelemetrySetting::Public;
}
std::vector<V6LapSummary> TnrdV6Archive::driverLapSummaries(uint8_t index) const {
    std::vector<V6LapSummary> out; for (const auto& lap : impl_->lapSummaries) if (lap.driverIndex == index) out.push_back(lap);
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        return std::tie(a.phase, a.startSessionTime, a.lapId) < std::tie(b.phase, b.startSessionTime, b.lapId);
    }); return out;
}
const std::vector<V6ChunkInfo>& TnrdV6Archive::v6Chunks() const { return impl_->v6Chunks; }
const std::vector<V6SharedRecord>& TnrdV6Archive::sharedRecords() const { return impl_->shared; }
float TnrdV6Archive::logicalTime(V6Phase phase, float time) const { return impl_->logical(phase, time); }
bool TnrdV6Archive::readDriverLapTypes(uint8_t driver, uint32_t lapId, const std::vector<uint8_t>& types,
                                       std::vector<V6TimedRow>& out, std::string* errorOut) {
    out.clear(); std::set<uint8_t> wanted(types.begin(), types.end());
    for (size_t i = 0; i < impl_->v6Chunks.size(); ++i) {
        const auto& chunk = impl_->v6Chunks[i];
        if (chunk.driverIndex != driver || chunk.lapId != lapId || (!wanted.empty() && !wanted.contains(chunk.typeId))) continue;
        std::shared_ptr<std::string> plain; if (!loadChunkPlain(i, plain, errorOut)) return false;
        if (!parseChunkRows(*plain, chunk, 0.0f, out,
                            -std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity())) return false;
    }
    std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        return std::tie(a.sessionTime, a.sequence, a.sourceOffset) < std::tie(b.sessionTime, b.sequence, b.sourceOffset);
    }); return true;
}
bool TnrdV6Archive::readDriverRangeTypes(uint8_t driver, float from, float to, const std::vector<uint8_t>& types,
                                         std::vector<V6TimedRow>& out, std::string* errorOut,
                                         const IndexedCancelCheck& cancelled) {
    out.clear(); std::set<uint8_t> wanted(types.begin(), types.end());
    for (size_t i = 0; i < impl_->v6Chunks.size(); ++i) {
        const auto& chunk = impl_->v6Chunks[i];
        if (chunk.phase != V6Phase::Race || chunk.driverIndex != driver ||
            (!wanted.empty() && !wanted.contains(chunk.typeId))) continue;
        constexpr float offset = 0.0f;
        if (chunk.lastTime + offset < from || chunk.firstTime + offset > to) continue;
        std::shared_ptr<std::string> plain; if (!loadChunkPlain(i, plain, errorOut)) return false;
        if (!parseChunkRows(*plain, chunk, offset, out, from, to, cancelled)) return false;
    }
    std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        return std::tie(a.sessionTime, a.sequence, a.sourceOffset) < std::tie(b.sessionTime, b.sequence, b.sourceOffset);
    }); return true;
}
bool TnrdV6Archive::readMultiDriverLatest(float at, const std::vector<uint8_t>& drivers,
                                           const std::vector<uint8_t>& types, std::vector<V6TimedRow>& out,
                                           std::string* errorOut) {
    out.clear();
    for (uint8_t driver : drivers) for (uint8_t type : types) {
        size_t latestChunk = impl_->v6Chunks.size();
        std::tuple<float, uint64_t> latestKey{-std::numeric_limits<float>::infinity(), 0};
        for (size_t index = 0; index < impl_->v6Chunks.size(); ++index) {
            const auto& chunk = impl_->v6Chunks[index];
            if (chunk.phase != V6Phase::Race || chunk.driverIndex != driver || chunk.typeId != type) continue;
            const float first = impl_->logical(chunk.phase, chunk.firstTime);
            if (first > at) continue;
            const auto key = std::tuple{first, chunk.sequence};
            if (latestChunk == impl_->v6Chunks.size() || key > latestKey) {
                latestChunk = index;
                latestKey = key;
            }
        }
        if (latestChunk == impl_->v6Chunks.size()) continue;

        std::vector<V6TimedRow> rows;
        std::shared_ptr<std::string> plain;
        if (!loadChunkPlain(latestChunk, plain, errorOut)) return false;
        const auto& chunk = impl_->v6Chunks[latestChunk];
        constexpr float offset = 0.0f;
        if (!parseChunkRows(*plain, chunk, offset, rows,
                            -std::numeric_limits<float>::infinity(), at)) return false;
        if (rows.empty()) continue;
        if (!stateType(static_cast<V6DataType>(type))) {
            out.push_back(std::move(rows.back()));
            continue;
        }

        // A state type can contain independently updated field groups. Tyre
        // state, for example, carries both compound/age and tyre-set allocation.
        // Restore the latest value of every group, not merely the final line in
        // the chunk, while still respecting an explicit unavailable transition.
        std::map<std::string, V6TimedRow> latest;
        for (auto& row : rows) {
            const bool unavailable = row.json.find("\"available\":false") != std::string::npos;
            if (unavailable) {
                latest.clear();
                latest.emplace("available", std::move(row));
                continue;
            }
            latest.erase("available");
            size_t cursor = 0;
            std::string signature;
            while ((cursor = row.json.find('"', cursor)) != std::string::npos) {
                const size_t end = row.json.find('"', cursor + 1);
                if (end == std::string::npos) break;
                const std::string_view key(row.json.data() + cursor + 1, end - cursor - 1);
                cursor = end + 1;
                if (key != "driver_idx" && key != "session_time" && key != "_v6_type") {
                    signature.assign(key);
                    break;
                }
            }
            if (!signature.empty()) latest.insert_or_assign(std::move(signature), std::move(row));
        }
        for (auto& [_, row] : latest) out.push_back(std::move(row));
    }
    std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        return std::tie(a.sessionTime, a.rowType, a.sequence) < std::tie(b.sessionTime, b.rowType, b.sequence);
    }); return true;
}

namespace TNRD_V6 {
bool load(const std::string& path, V6LoadResult& result, std::string& error) {
    auto archive = std::make_unique<TnrdV6Archive>(); HeaderRow header;
    if (!archive->open(path, header, &error)) return false;
    result.header = std::move(header); result.archive = std::move(archive); return true;
}
} // namespace TNRD_V6

} // namespace tnrp::detail
