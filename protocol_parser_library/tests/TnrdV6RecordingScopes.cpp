// A live session can be recorded to two files at once: every driver
// (…-all.tnrd) and the player alone (…-driver.tnrd). These cases cover that
// contract on the retained V6 memory writer: attaching the second file leaves
// the all-drivers file byte-identical to a single-file recording, the
// driver-only file holds no other car's chunks, laps or header but keeps the
// shared records, and both hold when the file is attached mid-session or
// rewritten after a flashback. RecordingScopes' session categories are
// checked too.

#ifdef _WIN32
// Without this the Debug CRT answers abort() with a modal dialog, which hangs
// an unattended run and hides everything the test printed.
#include <crtdbg.h>
#include <stdlib.h>
#include <windows.h>
#endif

#include "tnrd/TNRD_V6.h"
#include "tnrp/RecordingScope.h"
#include "tnrp/rows.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
using tnrp::detail::TnrdV6Archive;
using tnrp::detail::TnrdV6Writer;
using tnrp::detail::V6SourceRow;

int failures = 0;

void report(const char* what, int line, const char* expr) {
    std::printf("FAIL line %d: %s  (%s)\n", line, what, expr);
    std::fflush(stdout);
    ++failures;
}

#define CHECK(cond, what)                             \
    do {                                              \
        if (!(cond)) report((what), __LINE__, #cond); \
    } while (0)

#define REQUIRE(cond, what)                                       \
    do {                                                          \
        if (!(cond)) { report((what), __LINE__, #cond); return; } \
    } while (0)

void stage(const char* name) {
    std::printf("stage: %s\n", name);
    std::fflush(stdout);
}

constexpr uint8_t kPlayer = 0;
constexpr uint8_t kOther = 1;

std::string timingRow(float time, int lapNum, int lastLapMs, int sector) {
    std::string out = R"({"type":"timing","session_time":)" + std::to_string(time) +
        R"(,"player_idx":0,"cars":[)";
    for (int car = 0; car < 2; ++car) {
        if (car) out += ",";
        out += R"({"idx":)" + std::to_string(car) +
            R"(,"position":)" + std::to_string(car + 1) +
            R"(,"lap_num":)" + std::to_string(lapNum) +
            R"(,"current_lap_ms":0,"last_lap_ms":)" + std::to_string(lastLapMs) +
            R"(,"s1_ms":30000,"s2_ms":30000,"gap_ms":0,"pit_status":0,"num_pit_stops":0,)"
            R"("lap_invalid":false,"penalties_s":0,"num_dt_pens":0,"num_sg_pens":0,"sector":)" +
            std::to_string(sector) + R"(,"result_status":2,"driver_status":1})";
    }
    out += "]}";
    return out;
}

// The player's telemetry, plus the other car's: both become per-car chunks.
std::string telemetryRow(float time, int speed) {
    return R"({"type":"telemetry","session_time":)" + std::to_string(time) +
        R"(,"player_idx":0,"speed_kph":)" + std::to_string(speed) +
        R"(,"throttle":0.5,"brake":0,"steering":0,"gear":4,"rpm":9000,"drs":0,)"
        R"("rev_lights_pct":50,"rev_lights_bit_value":255,"cars":[)"
        R"({"idx":1,"speed_kph":)" + std::to_string(speed - 5) +
        R"(,"throttle":0.4,"brake":0,"steering":0,"gear":4,"rpm":8800,"drs":0}]})";
}

std::string participantsRow(float time) {
    return R"({"type":"participants","session_time":)" + std::to_string(time) +
        R"(,"player_idx":0,"drivers":[)"
        R"({"idx":0,"name":"ALPHA","team_id":1,"race_number":11,"your_telemetry":1},)"
        R"({"idx":1,"name":"BRAVO","team_id":2,"race_number":22,"your_telemetry":1}]})";
}

// Four 90-second laps for two cars, then enough session time for every lap to
// clear its write delay.
std::vector<V6SourceRow> buildRows() {
    std::vector<V6SourceRow> rows;
    rows.push_back({participantsRow(0.5f), 0.5f});
    float time = 1.0f;
    for (int lap = 1; lap <= 4; ++lap) {
        const int lastLapMs = lap == 1 ? 0 : 90000 + lap * 100;
        for (int sector = 0; sector < 3; ++sector) {
            rows.push_back({timingRow(time, lap, lastLapMs, sector), time});
            rows.push_back({telemetryRow(time + 0.1f, 200 + lap * 3 + sector), time + 0.1f});
            time += 30.0f;
        }
    }
    rows.push_back({timingRow(time + 120.0f, 5, 90500, 0), time + 120.0f});
    return rows;
}

fs::path testRoot;
tnrp::HeaderRow sourceHeader;

struct Plan {
    // Attach files before this row index; rows before it are the backlog.
    size_t attachAt = 0;
    // When set, rewind to this time after every row and replay the rows after it.
    float rewindTo = -1.0f;
};

// One memory session fed `rows`, recording to `allPath` and, when given,
// `driverPath` (player only).
bool record(const std::vector<V6SourceRow>& rows, const Plan& plan,
            const fs::path& allPath, const fs::path* driverPath, std::string& error) {
    TnrdV6Writer writer;
    if (!writer.openMemory(sourceHeader, &error)) return false;
    const auto attach = [&] {
        if (!writer.attachFile(allPath.string(), sourceHeader, &error)) return false;
        return !driverPath || writer.attachFile(driverPath->string(), sourceHeader, &error, true);
    };
    for (size_t i = 0; i < rows.size(); ++i) {
        if (i == plan.attachAt && !attach()) return false;
        if (!writer.appendRow(rows[i].line, rows[i].sessionTime, &error)) return false;
    }
    if (plan.attachAt >= rows.size() && !attach()) return false;
    if (plan.rewindTo >= 0.0f) {
        if (!writer.rewind(plan.rewindTo, &error)) return false;
        for (const auto& row : rows)
            if (row.sessionTime > plan.rewindTo &&
                !writer.appendRow(row.line, row.sessionTime, &error)) return false;
    }
    for (const auto& [path, why] : writer.takeFileErrors()) {
        error = path + ": " + why;
        return false;
    }
    return writer.finish(&error);
}

std::vector<char> bytesOf(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// Records the same session with one file and with two, then checks both.
void checkPair(const char* name, const Plan& plan) {
    stage(name);
    const auto rows = buildRows();
    const fs::path single = testRoot / (std::string(name) + "-single-all.tnrd");
    const fs::path all = testRoot / (std::string(name) + "-all.tnrd");
    const fs::path driver = testRoot / (std::string(name) + "-driver.tnrd");
    std::string error;
    REQUIRE(record(rows, plan, single, nullptr, error), error.c_str());
    REQUIRE(record(rows, plan, all, &driver, error), error.c_str());

    const auto singleBytes = bytesOf(single);
    CHECK(!singleBytes.empty(), "the single-file recording was written");
    CHECK(singleBytes == bytesOf(all),
          "a second, driver-only file leaves the all-drivers file byte-identical");

    TnrdV6Archive allArchive;
    tnrp::HeaderRow header;
    REQUIRE(allArchive.open(all.string(), header, &error), error.c_str());
    size_t allPlayerChunks = 0, allOtherChunks = 0;
    for (const auto& chunk : allArchive.v6Chunks())
        (chunk.driverIndex == kPlayer ? allPlayerChunks : allOtherChunks) += 1;
    CHECK(allOtherChunks > 0, "the all-drivers file holds the other car's chunks");
    CHECK(allArchive.driverHeaders().size() == 2, "the all-drivers file lists both drivers");

    TnrdV6Archive driverArchive;
    REQUIRE(driverArchive.open(driver.string(), header, &error), error.c_str());
    CHECK(!driverArchive.wasRecovered(), "the driver-only file has its index");
    CHECK(header.track_name == sourceHeader.track_name, "driver-only session header");
    size_t driverPlayerChunks = 0;
    for (const auto& chunk : driverArchive.v6Chunks()) {
        CHECK(chunk.driverIndex == kPlayer, "the driver-only file holds only the player's chunks");
        if (chunk.driverIndex == kPlayer) ++driverPlayerChunks;
    }
    CHECK(driverPlayerChunks == allPlayerChunks,
          "the driver-only file holds every chunk of the player's");
    REQUIRE(driverArchive.driverHeaders().size() == 1, "the driver-only file lists one driver");
    CHECK(driverArchive.driverHeaders()[0].vehicleIndex == kPlayer, "that driver is the player");
    CHECK(driverArchive.playerDriverIndex() == kPlayer, "the player is still the player");
    CHECK(driverArchive.driverLapSummaries(kOther).empty(), "no laps of the other car");
    CHECK(driverArchive.driverLapSummaries(kPlayer).size() ==
              allArchive.driverLapSummaries(kPlayer).size(),
          "every lap of the player's");
    bool otherNamed = false;
    for (const auto& record : driverArchive.sharedRecords())
        if (record.json.find("BRAVO") != std::string::npos) otherNamed = true;
    CHECK(driverArchive.sharedRecords().size() == allArchive.sharedRecords().size(),
          "the driver-only file keeps every shared record");
    CHECK(otherNamed, "the shared participants still name the other car");
    for (size_t i = 0; i < driverArchive.v6Chunks().size(); ++i) {
        std::shared_ptr<std::string> plain;
        if (!driverArchive.loadChunkPlain(i, plain, &error)) {
            report(error.c_str(), __LINE__, "loadChunkPlain");
            return;
        }
    }
    std::printf("  all: %zu player + %zu other chunks; driver: %zu chunks\n",
                allPlayerChunks, allOtherChunks, driverPlayerChunks);
    std::fflush(stdout);
}

void caseSessionCategories() {
    stage("session types map to their categories");
    tnrp::RecordingScopes scopes;
    CHECK(scopes.forSession(15) == tnrp::RecordingScope::AllDrivers, "everything defaults to all drivers");
    scopes.practice = tnrp::RecordingScope::DriverOnly;
    scopes.qualifying = tnrp::RecordingScope::Both;
    scopes.race = tnrp::RecordingScope::Ask;
    scopes.timeTrial = tnrp::RecordingScope::DriverOnly;
    for (int type = 1; type <= 4; ++type)
        CHECK(scopes.forSession(type) == tnrp::RecordingScope::DriverOnly, "1-4 are practice");
    for (int type = 5; type <= 14; ++type)
        CHECK(scopes.forSession(type) == tnrp::RecordingScope::Both, "5-14 are qualifying");
    for (int type = 15; type <= 17; ++type)
        CHECK(scopes.forSession(type) == tnrp::RecordingScope::Ask, "15-17 are race");
    CHECK(scopes.forSession(18) == tnrp::RecordingScope::DriverOnly, "18 is time trial");
    CHECK(scopes.forSession(0) == tnrp::RecordingScope::AllDrivers, "an unknown session keeps every driver");
    for (auto scope : {tnrp::RecordingScope::AllDrivers, tnrp::RecordingScope::DriverOnly,
                       tnrp::RecordingScope::Both, tnrp::RecordingScope::Ask})
        CHECK(tnrp::recordingScopeFromString(tnrp::toString(scope)) == scope, "scope names round-trip");
    CHECK(tnrp::recordingScopeFromString("bogus") == tnrp::RecordingScope::AllDrivers,
          "an unknown scope name keeps every driver");
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
            ("tnrd_v6_recording_scopes_" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(testRoot);

        sourceHeader.protocol = 2026;
        sourceHeader.track_id = 16;
        sourceHeader.track_name = "Scope Test Track";
        sourceHeader.track_length_m = 4300;
        sourceHeader.formula = 0;
        sourceHeader.session_type = 15;
        sourceHeader.session_name = "Race";
        sourceHeader.start_time = 123456789;

        caseSessionCategories();
        checkPair("from-start", Plan{});
        // Laps 1 and 2 are committed before the files attach: the backlog
        // must be filtered as the live commits are.
        checkPair("mid-session", Plan{buildRows().size() / 2, -1.0f});
        // Back into committed lap 3: both files are rewritten from memory.
        checkPair("flashback", Plan{0, 200.0f});

        std::error_code ignored;
        fs::remove_all(testRoot, ignored);
    } catch (const std::exception& e) {
        std::printf("EXCEPTION: %s\n", e.what());
        std::fflush(stdout);
        return 1;
    }

    if (failures) {
        std::printf("TnrdV6RecordingScopes: %d check(s) failed\n", failures);
        std::fflush(stdout);
        return 1;
    }
    std::printf("TnrdV6RecordingScopes: all cases passed\n");
    std::fflush(stdout);
    return 0;
}
