// TNRD V6 writes its index once, when the recording closes. A recording
// interrupted before that has no index, so the reader rebuilds one by scanning
// the append-only record stream. These cases cover that contract: a torn file
// still opens, what it rebuilds matches what the writer would have written, and
// a file that is not V6 at all is still rejected.
// See docs/TNRD_V6_WRITER_EFFICIENCY_DESIGN.md section 3.6.

#ifdef _WIN32
// Without this the Debug CRT answers abort() with a modal dialog, which hangs
// an unattended run and hides everything the test printed.
#include <crtdbg.h>
#include <stdlib.h>
#include <windows.h>
#endif

#include "tnrd/TNRD_V6.h"
#include "tnrp/rows.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using tnrp::detail::TnrdV6Archive;
using tnrp::detail::V6SourceRow;

int failures = 0;

void report(const char* what, int line, const char* expr) {
    std::printf("FAIL line %d: %s  (%s)\n", line, what, expr);
    std::fflush(stdout);
    ++failures;
}

// Records a failure and keeps going where the case can still yield information,
// so one broken assumption does not hide the rest.
#define CHECK(cond, what)                             \
    do {                                              \
        if (!(cond)) report((what), __LINE__, #cond); \
    } while (0)

// Stops the current case: continuing past this would only produce noise.
#define REQUIRE(cond, what)                                       \
    do {                                                          \
        if (!(cond)) { report((what), __LINE__, #cond); return; } \
    } while (0)

void stage(const char* name) {
    std::printf("stage: %s\n", name);
    std::fflush(stdout);
}

std::string timingRow(float time, int lapNum, int lastLapMs, int s1Ms, int s2Ms,
                      bool invalid, int sector) {
    std::string out = R"({"type":"timing","session_time":)" + std::to_string(time) +
        R"(,"player_idx":0,"cars":[)";
    for (int car = 0; car < 2; ++car) {
        if (car) out += ",";
        out += R"({"idx":)" + std::to_string(car) +
            R"(,"position":)" + std::to_string(car + 1) +
            R"(,"lap_num":)" + std::to_string(lapNum) +
            R"(,"current_lap_ms":0,"last_lap_ms":)" + std::to_string(lastLapMs) +
            R"(,"s1_ms":)" + std::to_string(s1Ms) +
            R"(,"s2_ms":)" + std::to_string(s2Ms) +
            R"(,"gap_ms":0,"pit_status":0,"num_pit_stops":0,"lap_invalid":)" +
            (invalid ? "true" : "false") +
            R"(,"penalties_s":0,"num_dt_pens":0,"num_sg_pens":0,"sector":)" +
            std::to_string(sector) + R"(,"result_status":2,"driver_status":1})";
    }
    out += "]}";
    return out;
}

std::string telemetryRow(float time, int speed) {
    return R"({"type":"telemetry","session_time":)" + std::to_string(time) +
        R"(,"speed_kph":)" + std::to_string(speed) +
        R"(,"throttle":0.5,"brake":0,"steer":0,"gear":4,"rpm":9000,"drs":0,)"
        R"("rev_lights_pct":50,"rev_lights_bit_value":255})";
}

std::string participantsRow(float time) {
    return R"({"type":"participants","session_time":)" + std::to_string(time) +
        R"(,"player_idx":0,"drivers":[)"
        R"({"idx":0,"name":"ALPHA","team_id":1,"race_number":11,"your_telemetry":1},)"
        R"({"idx":1,"name":"BRAVO","team_id":2,"race_number":22,"your_telemetry":1}]})";
}

// Four laps for two cars. A lap's time and sectors arrive on the first sample of
// the following lap, which is the derivation the recovery scan must reproduce.
std::vector<V6SourceRow> buildRows() {
    std::vector<V6SourceRow> rows;
    rows.push_back({participantsRow(0.5f), 0.5f});
    float time = 1.0f;
    for (int lap = 1; lap <= 4; ++lap) {
        const int lastLapMs = lap == 1 ? 0 : 90000 + lap * 100;
        rows.push_back({timingRow(time, lap, lastLapMs, 30000, 30000, false, 0), time});
        rows.push_back({telemetryRow(time + 0.1f, 200 + lap), time + 0.1f});
        time += 30.0f;
        rows.push_back({timingRow(time, lap, lastLapMs, 30000, 30000, lap == 3, 1), time});
        rows.push_back({telemetryRow(time + 0.1f, 210 + lap), time + 0.1f});
        time += 30.0f;
        rows.push_back({timingRow(time, lap, lastLapMs, 30000, 30000, lap == 3, 2), time});
        rows.push_back({telemetryRow(time + 0.1f, 220 + lap), time + 0.1f});
        time += 30.0f;
    }
    // Session time must run well past the final lap so every lap clears its
    // 30-second write delay and reaches the file.
    rows.push_back({timingRow(time + 120.0f, 5, 90500, 30000, 30000, false, 0), time + 120.0f});
    return rows;
}

uint64_t readU64(const unsigned char* p) {
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) value |= static_cast<uint64_t>(p[i]) << (8 * i);
    return value;
}

// The header's metadataOffset: where the trailing index begins.
uint64_t indexOffset(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return 0;
    unsigned char header[128]{};
    in.read(reinterpret_cast<char*>(header), sizeof(header));
    if (in.gcount() != static_cast<std::streamsize>(sizeof(header))) return 0;
    return readU64(header + 16);
}

bool copyTruncated(const fs::path& from, const fs::path& to, uint64_t bytes) {
    std::ifstream in(from, std::ios::binary);
    std::ofstream out(to, std::ios::binary | std::ios::trunc);
    if (!in || !out) return false;
    std::vector<char> buffer(64 * 1024);
    uint64_t written = 0;
    while (written < bytes) {
        const auto want = static_cast<std::streamsize>(
            std::min<uint64_t>(buffer.size(), bytes - written));
        in.read(buffer.data(), want);
        const auto got = in.gcount();
        if (got <= 0) break;
        out.write(buffer.data(), got);
        written += static_cast<uint64_t>(got);
    }
    return true;
}

struct Snapshot {
    std::vector<tnrp::detail::V6LapSummary> laps;
    std::vector<tnrp::detail::V6ChunkInfo> chunks;
    size_t driverCount = 0;
};

Snapshot snapshotOf(const TnrdV6Archive& archive) {
    Snapshot out;
    out.chunks = archive.v6Chunks();
    out.driverCount = archive.driverHeaders().size();
    for (const auto& header : archive.driverHeaders())
        for (const auto& lap : archive.driverLapSummaries(header.vehicleIndex))
            out.laps.push_back(lap);
    return out;
}

bool sameLap(const tnrp::detail::V6LapSummary& a, const tnrp::detail::V6LapSummary& b) {
    if (a.lapId == b.lapId && a.driverIndex == b.driverIndex &&
        a.lapNumber == b.lapNumber && a.phase == b.phase &&
        a.lapTimeMs == b.lapTimeMs && a.s1Ms == b.s1Ms && a.s2Ms == b.s2Ms &&
        a.s3Ms == b.s3Ms && a.isCompleted == b.isCompleted &&
        a.isValid == b.isValid && a.isPartial == b.isPartial) {
        return true;
    }
    std::printf("  lap mismatch recovered/written: id=%u/%u driver=%u/%u num=%u/%u "
                "time=%u/%u s1=%u/%u s2=%u/%u s3=%u/%u done=%d/%d valid=%d/%d partial=%d/%d\n",
                a.lapId, b.lapId, unsigned(a.driverIndex), unsigned(b.driverIndex),
                a.lapNumber, b.lapNumber, a.lapTimeMs, b.lapTimeMs,
                a.s1Ms, b.s1Ms, a.s2Ms, b.s2Ms, a.s3Ms, b.s3Ms,
                int(a.isCompleted), int(b.isCompleted), int(a.isValid), int(b.isValid),
                int(a.isPartial), int(b.isPartial));
    std::fflush(stdout);
    return false;
}

fs::path testRoot;
fs::path intactPath;
tnrp::HeaderRow sourceHeader;
Snapshot reference;

// 1. The intact recording opens through its index, not the scan.
void caseIntactOpensNormally() {
    stage("intact file opens through its index");
    TnrdV6Archive archive;
    tnrp::HeaderRow out;
    std::string error;
    REQUIRE(archive.open(intactPath.string(), out, &error), error.c_str());
    CHECK(!archive.wasRecovered(), "an intact file must not need recovery");
    CHECK(out.track_name == sourceHeader.track_name, "session header round trip");
    reference = snapshotOf(archive);
    CHECK(!reference.chunks.empty(), "intact file has chunks");
    CHECK(!reference.laps.empty(), "intact file has laps");
    std::printf("  intact: %zu chunks, %zu laps, %zu drivers\n",
                reference.chunks.size(), reference.laps.size(), reference.driverCount);
    std::fflush(stdout);
}

// 2. Truncating exactly at the index start is the crash this design exists for:
//    every chunk reached disk, only the index did not. What the scan rebuilds
//    must match what the writer wrote.
void caseIndexMissingRebuildsIdentically() {
    stage("index-start truncation rebuilds identical metadata");
    const uint64_t index = indexOffset(intactPath);
    REQUIRE(index > 128, "intact file carries an index offset");
    const fs::path torn = testRoot / "no_index.tnrd";
    REQUIRE(copyTruncated(intactPath, torn, index), "could truncate a copy");

    TnrdV6Archive archive;
    tnrp::HeaderRow out;
    std::string error;
    REQUIRE(archive.open(torn.string(), out, &error), error.c_str());
    CHECK(archive.wasRecovered(), "an index-less file must be recovered by scan");
    CHECK(out.track_name == sourceHeader.track_name, "recovered track name");
    CHECK(out.protocol == sourceHeader.protocol, "recovered protocol");
    CHECK(out.session_type == sourceHeader.session_type, "recovered session type");

    const Snapshot recovered = snapshotOf(archive);
    CHECK(recovered.driverCount == reference.driverCount, "recovered driver count");
    if (recovered.chunks.size() != reference.chunks.size()) {
        std::printf("  chunks: %zu recovered vs %zu written\n",
                    recovered.chunks.size(), reference.chunks.size());
        std::fflush(stdout);
    }
    REQUIRE(recovered.chunks.size() == reference.chunks.size(), "recovered chunk count");
    for (size_t i = 0; i < recovered.chunks.size(); ++i) {
        const auto& a = recovered.chunks[i];
        const auto& b = reference.chunks[i];
        CHECK(a.driverIndex == b.driverIndex, "chunk driver");
        CHECK(a.lapId == b.lapId, "chunk lap id");
        CHECK(a.typeId == b.typeId, "chunk data type");
        CHECK(a.phase == b.phase, "chunk phase");
        CHECK(a.offset == b.offset, "chunk payload offset");
        CHECK(a.compressedSize == b.compressedSize, "chunk compressed size");
        CHECK(a.uncompressedSize == b.uncompressedSize, "chunk plain size");
        CHECK(a.sampleCount == b.sampleCount, "chunk sample count");
        CHECK(a.checksum == b.checksum, "chunk checksum");
        CHECK(a.firstTime == b.firstTime, "chunk first sample time");
        CHECK(a.lastTime == b.lastTime, "chunk last sample time");
    }
    if (recovered.laps.size() != reference.laps.size()) {
        std::printf("  laps: %zu recovered vs %zu written\n",
                    recovered.laps.size(), reference.laps.size());
        std::fflush(stdout);
    }
    REQUIRE(recovered.laps.size() == reference.laps.size(), "recovered lap count");
    for (size_t i = 0; i < recovered.laps.size(); ++i)
        CHECK(sameLap(recovered.laps[i], reference.laps[i]), "lap summary identity");
}

// 3. A recovered archive must answer payload reads, not merely open.
void caseRecoveredArchiveReadsPayloads() {
    stage("recovered archive reads its payloads");
    const uint64_t index = indexOffset(intactPath);
    REQUIRE(index > 128, "intact file carries an index offset");
    const fs::path torn = testRoot / "read_back.tnrd";
    REQUIRE(copyTruncated(intactPath, torn, index), "could truncate a copy");

    TnrdV6Archive archive;
    tnrp::HeaderRow out;
    std::string error;
    REQUIRE(archive.open(torn.string(), out, &error), error.c_str());
    REQUIRE(archive.wasRecovered(), "read-back file must be recovered");
    for (size_t i = 0; i < archive.v6Chunks().size(); ++i) {
        std::shared_ptr<std::string> plain;
        if (!archive.loadChunkPlain(i, plain, &error)) {
            report(error.c_str(), __LINE__, "loadChunkPlain");
            return;
        }
        CHECK(plain && !plain->empty(), "recovered chunk payload is readable");
    }
}

// 4. Truncation anywhere: inside a prefix, inside a payload, between records,
//    inside the index. Every offset must open or fail cleanly, never crash and
//    never report success with nothing behind it.
void caseTruncationSweep() {
    stage("truncation sweep across the whole file");
    const uint64_t total = fs::file_size(intactPath);
    const fs::path torn = testRoot / "sweep.tnrd";
    int opened = 0;
    int rejected = 0;
    for (uint64_t cut = 128; cut <= total; cut += 97) {
        if (!copyTruncated(intactPath, torn, cut)) {
            report("could truncate a copy", __LINE__, "copyTruncated");
            return;
        }
        TnrdV6Archive archive;
        tnrp::HeaderRow out;
        std::string error;
        if (archive.open(torn.string(), out, &error)) {
            CHECK(!archive.v6Chunks().empty(), "an opened file must carry chunks");
            ++opened;
        } else {
            CHECK(!error.empty(), "a rejection must carry a reason");
            ++rejected;
        }
    }
    std::error_code ignored;
    fs::remove(torn, ignored);
    std::printf("  sweep: %d opened, %d rejected\n", opened, rejected);
    std::fflush(stdout);
    CHECK(opened > 0, "cuts past the first chunk must recover");
    CHECK(opened + rejected > 10, "sweep must cover many offsets");
}

// 5. A file that is not V6 stays a hard failure. Recovery must never be
//    attempted on an arbitrary file.
void caseNonV6Rejected() {
    stage("a non-V6 file is still rejected");
    const fs::path alien = testRoot / "not_v6.bin";
    {
        std::ofstream out(alien, std::ios::binary | std::ios::trunc);
        const std::string junk(4096, 'x');
        out.write(junk.data(), static_cast<std::streamsize>(junk.size()));
    }
    TnrdV6Archive archive;
    tnrp::HeaderRow out;
    std::string error;
    CHECK(!archive.open(alien.string(), out, &error), "a non-V6 file must be rejected");
    CHECK(!error.empty(), "a non-V6 rejection must carry a reason");
}

// 6. Only the sentinel header, as a recorder that died before its first lap
//    would leave: rejected with a reason, not silently opened empty.
void caseHeaderOnlyRejected() {
    stage("a header-only file is rejected, not opened empty");
    const fs::path empty = testRoot / "header_only.tnrd";
    REQUIRE(copyTruncated(intactPath, empty, 128), "could truncate a copy");
    TnrdV6Archive archive;
    tnrp::HeaderRow out;
    std::string error;
    CHECK(!archive.open(empty.string(), out, &error), "a header-only file must be rejected");
    CHECK(!error.empty(), "a header-only rejection must carry a reason");
}

} // namespace

int main() {
#ifdef _WIN32
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#endif
    try {
        testRoot = fs::temp_directory_path() /
            ("tnrd_v6_recovery_" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(testRoot);
        intactPath = testRoot / "intact.tnrd";

        sourceHeader.protocol = 2026;
        sourceHeader.track_id = 16;
        sourceHeader.track_name = "Recovery Test Track";
        sourceHeader.track_length_m = 4300;
        sourceHeader.formula = 0;
        sourceHeader.session_type = 10;
        sourceHeader.session_name = "Race";
        sourceHeader.start_time = 123456789;

        stage("write a reference recording");
        std::string error;
        if (!tnrp::detail::writeTnrdV6(intactPath.string(), sourceHeader, buildRows(), &error)) {
            report(error.c_str(), __LINE__, "writeTnrdV6");
            return 1;
        }

        caseIntactOpensNormally();
        caseIndexMissingRebuildsIdentically();
        caseRecoveredArchiveReadsPayloads();
        caseTruncationSweep();
        caseNonV6Rejected();
        caseHeaderOnlyRejected();

        std::error_code ignored;
        fs::remove_all(testRoot, ignored);
    } catch (const std::exception& e) {
        std::printf("EXCEPTION: %s\n", e.what());
        std::fflush(stdout);
        return 1;
    }

    if (failures) {
        std::printf("TnrdV6Recovery: %d check(s) failed\n", failures);
        std::fflush(stdout);
        return 1;
    }
    std::printf("TnrdV6Recovery: all cases passed\n");
    std::fflush(stdout);
    return 0;
}
