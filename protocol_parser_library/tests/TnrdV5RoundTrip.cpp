#include "tnrd/TNRD_V5.h"
#include "TnrdCodec.h"
#include "tnrp/BinaryRows.h"
#include "tnrp/rows.h"
#include "tnrp/TnrdReader.h"

#include <algorithm>
#include <array>
#include <barrier>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

int main() {
    namespace fs = std::filesystem;
    using tnrp::detail::V5SourceRow;
    const fs::path root = fs::temp_directory_path() /
        ("tnrd_v5_roundtrip_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    const fs::path longDir = root / std::string(190, 'a');
#ifdef _WIN32
    fs::create_directories(fs::path(tnrp::detail::windowsExtendedPath(longDir.string())));
#else
    fs::create_directories(longDir);
#endif
    const fs::path path = longDir /
        "recording_with_a_deliberately_long_filename_for_extended_windows_path_testing.tnrd";
#ifdef _WIN32
    assert(path.string().size() > 260);
#endif

    tnrp::HeaderRow header;
    header.protocol = 2026;
    header.track_id = 7;
    header.track_name = "Test Track";
    header.track_length_m = 5000;
    header.formula = 13;
    header.session_type = 10;
    header.session_name = "Race";
    header.start_time = 123456789;

    const std::vector<V5SourceRow> rows = {
        {R"({"type":"lap","session_time":1,"lap_num":1,"current_lap_ms":0,"last_lap_ms":0,"lap_distance_m":0})", 1},
        {R"({"type":"telemetry","session_time":1.1,"speed_kph":100,"throttle":0.5,"brake":0,"steer":0,"gear":3,"rpm":9000,"drs":0,"rev_lights_pct":50})", 1.1f},
        {R"({"type":"positions","ts":"2026-08-11T00:00:01.150Z","player_idx":0,"cars":[{"idx":0,"x":10,"z":20}]})", 1.15f},
        {R"({"type":"status","session_time":1.2,"fuel_kg":50,"ers_pct":80,"tyre_compound":16,"visual_compound":16})", 1.2f},
        {R"({"type":"race_event","session_time":2,"code":"SCAR","event_type":3})" "\n", 2.0f},
        // Lap packets are menu-rate snapshots, so the first row of a new lap
        // commonly arrives one frame after the true boundary.
        {R"({"type":"lap","session_time":91.016,"lap_num":2,"current_lap_ms":16,"last_lap_ms":90000,"lap_distance_m":0})", 91.016f},
        {R"({"type":"telemetry","session_time":91.1,"speed_kph":110,"throttle":0.6,"brake":0,"steer":0,"gear":4,"rpm":9500,"drs":1,"rev_lights_pct":60})", 91.1f},
        {R"({"type":"positions","ts":"2026-08-11T00:01:31.150Z","player_idx":0,"cars":[{"idx":0,"x":30,"z":40}]})", 91.15f},
    };

    std::string error;
    assert(tnrp::detail::writeTnrdV5(path.string(), header, rows, &error));

    // Opening V5 is control-plane-only. No payload is decompressed until a
    // row-family query actually requests a chunk.
    tnrp::detail::TnrdV5Archive archive;
    tnrp::HeaderRow archiveHeader;
    assert(archive.open(path.string(), archiveHeader, &error));
    assert(archive.decompressedChunkCount() == 0);
    assert(!archive.chunks().empty());
    const uint64_t firstPayloadOffset = archive.chunks().front().offset;
    std::vector<tnrp::detail::V5TimedRow> lazyRows;
    assert(archive.rowsForLap(1, tnrp::detail::v5TypeBit(1), lazyRows, &error));
    assert(archive.decompressedChunkCount() == 1);
    const uint64_t afterFirstRead = archive.decompressedChunkCount();
    assert(archive.rowsForLap(1, tnrp::detail::v5TypeBit(1), lazyRows, &error));
    assert(archive.decompressedChunkCount() == afterFirstRead); // LRU hit
    archive.setCacheLimitBytes(1);
    assert(archive.cacheBytes() <= 1);
    archive.close();

    // Chunk payloads are loaded concurrently, while requests that converge
    // on one chunk share its single in-flight read/decompression. Use a payload
    // larger than the deliberately tiny cache so this exercises single-flight,
    // not an LRU hit after the first caller finishes.
    const fs::path parallelPath = longDir / "parallel_channel_reader.tnrd";
    std::string largeTelemetry =
        R"({"type":"telemetry","session_time":1.1,"speed_kph":100,"padding":")";
    largeTelemetry.append(8 * 1024 * 1024, 'x');
    largeTelemetry += R"("})";
    const std::vector<V5SourceRow> parallelRows = {
        {R"({"type":"lap","session_time":1,"lap_num":1,"current_lap_ms":0,"last_lap_ms":0})", 1.0f},
        {std::move(largeTelemetry), 1.1f},
        {R"({"type":"status","session_time":1.2})", 1.2f},
        {R"({"type":"damage","session_time":1.3})", 1.3f},
        {R"({"type":"session","session_time":1.4})", 1.4f},
        {R"({"type":"race_event","session_time":1.5,"code":"TEST"})", 1.5f},
        {R"({"type":"timing","session_time":1.6})", 1.6f},
        {R"({"type":"participants","session_time":1.7})", 1.7f},
        {R"({"type":"all_status","session_time":1.8})", 1.8f},
        {R"({"type":"tyre_sets","session_time":1.9})", 1.9f},
        {R"({"type":"motion","session_time":2.0})", 2.0f},
    };
    assert(tnrp::detail::writeTnrdV5(parallelPath.string(), header, parallelRows, &error));
    tnrp::detail::TnrdV5Archive parallel;
    assert(parallel.open(parallelPath.string(), archiveHeader, &error));
    parallel.setCacheLimitBytes(1);
    const uint64_t beforeSharedRead = parallel.decompressedChunkCount();
    std::barrier startTogether(9);
    std::array<std::thread, 8> readers;
    std::array<bool, 8> readerOk{};
    for (size_t i = 0; i < readers.size(); ++i) {
        readers[i] = std::thread([&, i] {
            std::vector<tnrp::detail::V5TimedRow> sharedRows;
            std::string threadError;
            startTogether.arrive_and_wait();
            readerOk[i] = parallel.rowsForLap(
                1, tnrp::detail::v5TypeBit(1), sharedRows, &threadError) &&
                sharedRows.size() == 1 &&
                sharedRows.front().json.find("\"speed_kph\":100") != std::string::npos;
        });
    }
    startTogether.arrive_and_wait();
    for (auto& readerThread : readers) readerThread.join();
    assert(std::all_of(readerOk.begin(), readerOk.end(), [](bool ok) { return ok; }));
    assert(parallel.decompressedChunkCount() == beforeSharedRead + 1);
    std::vector<tnrp::detail::V5TimedRow> parallelRead;
    assert(parallel.rowsForRange(0, 3, 0xFFFFFFFFu, parallelRead, &error));
    assert(parallelRead.size() == parallelRows.size());
    assert(std::is_sorted(parallelRead.begin(), parallelRead.end(), [](const auto& a, const auto& b) {
        return a.sessionTime < b.sessionTime ||
            (a.sessionTime == b.sessionTime && a.sequence < b.sequence);
    }));
    std::vector<tnrp::detail::V5TimedRow> latestParallel;
    assert(parallel.latestRows(3, {1,2,3,4,5,6,7,8,9,10,11}, latestParallel, &error));
    assert(latestParallel.size() == parallelRows.size());
    size_t callbackChunks = 0;
    assert(parallel.forEachChunk(0xFFFFFFFFu,
        [&](const tnrp::detail::V5ChunkInfo&, std::string_view) {
            ++callbackChunks;
            return true;
        }, &error));
    assert(callbackChunks == parallel.chunks().size());
    parallel.close();

    tnrp::HeaderRow loadedHeader;
    tnrp::detail::TnrdV5Archive complete;
    assert(complete.open(path.string(), loadedHeader, &error));
    std::vector<tnrp::detail::V5TimedRow> completeRows;
    assert(complete.rowsForRange(complete.startTime(),complete.totalTime(),0xFFFFFFFFu,completeRows,&error));
    assert(loadedHeader.magic == "TNRD_V5");
    assert(loadedHeader.track_length_m.value_or(0) == 5000);
    assert(loadedHeader.formula.value_or(-1) == 13);
    assert(complete.laps().size() == 2);
    assert(complete.laps()[0].endSessionTime == complete.laps()[1].startSessionTime);
    assert(std::any_of(completeRows.begin(),completeRows.end(),[](const auto&r){return r.json.find("\"speed_kph\":110")!=std::string::npos;}));
    assert(std::any_of(completeRows.begin(),completeRows.end(),[](const auto&r){return r.rowType==13&&r.sessionTime==1.15f&&r.json.find("\"session_time\":")!=std::string::npos;}));
    std::vector<tnrp::detail::V5TimedRow> positionWindow;
    assert(complete.rowsForRange(1.14f, 1.16f, tnrp::detail::v5TypeBit(13), positionWindow, &error));
    assert(positionWindow.size() == 1 && positionWindow.front().sessionTime == 1.15f);
    complete.close();

    tnrp::TnrdReader reader;
    reader.setBinaryPlayback(true);
    assert(reader.load(path.string(), loadedHeader));
    assert(reader.loadedFormat() == tnrp::TnrdFormat::ChunkedV5);
    assert(reader.lapBlocksMessage().find("\"tnrdVersion\":\"TNRD_V5\"") != std::string::npos);
    assert(reader.lapBlocksMessage().find("\"deltaAvailable\":true") != std::string::npos);
    assert(reader.lapBlocksMessage().find('\n') == std::string::npos);
    glz::generic lapBlocksJson;
    assert(!glz::read_json(lapBlocksJson, reader.lapBlocksMessage()));
    assert(reader.readRange(0, 200).size() == rows.size());
    const auto derivedStrategy = reader.strategySnapshotAt(91.15f);
    assert(derivedStrategy.type == "strategy");
    assert(derivedStrategy.state == "non_race");
    assert(std::none_of(rows.begin(), rows.end(), [](const auto& r) {
        return r.line.find("\"type\":\"strategy\"") != std::string::npos;
    }));
    tnrp::PlaybackLapDataRow selectiveLap;
    assert(!glz::read_json(selectiveLap, reader.getLapDataMessage(
        1, tnrp::detail::v5TypeBit(1) | tnrp::detail::v5TypeBit(4))));
    assert(selectiveLap.rowTypeMask ==
        (tnrp::detail::v5TypeBit(1) | tnrp::detail::v5TypeBit(4)));
    assert(selectiveLap.telemetry.size() == 1);
    assert(selectiveLap.statusHistory.empty());
    assert(selectiveLap.motionHistory.empty());
    assert(selectiveLap.damageHistory.empty());
    assert(!selectiveLap.lapProgress.empty());

    // Packed hot-row filtering is record-aware: selecting Motion must not copy
    // the adjacent Telemetry record or split either record.
    std::vector<uint8_t> packed;
    TelemetryRow telemetryRow;
    telemetryRow.session_time = 1.0f;
    MotionRow motionRow;
    motionRow.session_time = 1.0f;
    tnrp::bin::encodeTelemetry(packed, telemetryRow);
    tnrp::bin::encodeMotion(packed, motionRow);
    std::vector<uint8_t> filtered;
    assert(tnrp::bin::appendFilteredBatch(
        filtered, packed.data(), packed.size(), tnrp::detail::v5TypeBit(11)));
    size_t filteredRows = 0;
    assert(tnrp::bin::forEachPackedRecord(filtered.data(), filtered.size(),
        [&](uint8_t rowType, const uint8_t*, size_t) {
            assert(rowType == 11);
            ++filteredRows;
        }));
    assert(filteredRows == 1);
    const auto finiteWindow = reader.seekFlush(
        91.15f, 91.0f, false, tnrp::detail::v5TypeBit(2), 100.0f, false);
    assert(finiteWindow.coldJson.find("\"fuel_kg\":50") != std::string::npos);
    const fs::path xlsxPath = longDir /
        "export_with_a_deliberately_long_filename_for_extended_windows_path_testing.xlsx";
    assert(reader.exportXlsx(loadedHeader, xlsxPath.string(), &error));
#ifdef _WIN32
    assert(fs::exists(fs::path(tnrp::detail::windowsExtendedPath(xlsxPath.string()))));
#else
    assert(fs::exists(xlsxPath));
#endif
    reader.close();

    // Incremental checkpoints must append only new delta chunks. Repeated
    // checkpoints for the same (lap,type) are valid distinct segments, and
    // finalization must remain a small metadata/tail operation.
    const fs::path incrementalPath = longDir / "incremental_append_only_writer.tnrd";
    tnrp::detail::TnrdV5Writer incremental;
    assert(incremental.open(incrementalPath.string(), header, &error));
    size_t expectedRows = 0;
    const std::string chunkPadding(32 * 1024, 'p');
    for (int batch = 0; batch < 12; ++batch) {
        std::vector<V5SourceRow> delta;
        if (batch == 0) {
            delta.push_back({R"({"type":"lap","session_time":1,"lap_num":1,"current_lap_ms":0,"last_lap_ms":0,"lap_distance_m":0})", 1});
        }
        for (int i = 0; i < 25; ++i) {
            const float t = 1.1f + batch * 2.5f + i * 0.1f;
            delta.push_back({"{\"type\":\"telemetry\",\"session_time\":" + std::to_string(t) +
                ",\"speed_kph\":100,\"throttle\":0.5,\"brake\":0,\"steer\":0,\"gear\":3,\"rpm\":9000,\"drs\":0,\"rev_lights_pct\":50,\"padding\":\"" + chunkPadding + "\"}", t});
        }
        expectedRows += delta.size();
        assert(incremental.append(delta, &error));
        assert(incremental.checkpoint(&error));
    }
#ifdef _WIN32
    const auto incrementalFsPath = fs::path(tnrp::detail::windowsExtendedPath(incrementalPath.string()));
#else
    const auto incrementalFsPath = incrementalPath;
#endif
    const auto beforeFinish = fs::file_size(incrementalFsPath);
    assert(incremental.finish(&error));
    const auto finishGrowth = fs::file_size(incrementalFsPath) - beforeFinish;
    assert(finishGrowth < 64 * 1024); // directory/footer only; never a session rewrite
    tnrp::detail::TnrdV5Archive incrementalRead;assert(incrementalRead.open(incrementalPath.string(),loadedHeader,&error));completeRows.clear();assert(incrementalRead.rowsForRange(incrementalRead.startTime(),incrementalRead.totalTime(),0xFFFFFFFFu,completeRows,&error));assert(completeRows.size()==expectedRows);assert(incrementalRead.peakConcurrentChunkLoads()>=2);assert(incrementalRead.peakConcurrentChunkLoads()<=8);incrementalRead.close();

    // Formation-lap/session-scope rows remain addressable as logical lap 0,
    // and a flashback behind committed data supersedes only affected chunks.
    const fs::path flashbackPath = longDir / "formation_lap_flashback.tnrd";
    tnrp::detail::TnrdV5Writer flashback;
    assert(flashback.open(flashbackPath.string(), header, &error));
    assert(flashback.append({
        {R"({"type":"session","session_time":0.5,"session_type":10})", 0.5f},
        {R"({"type":"lap","session_time":1,"lap_num":1,"current_lap_ms":0,"last_lap_ms":0,"lap_distance_m":0})", 1.0f},
        {R"({"type":"telemetry","session_time":40,"speed_kph":200})", 40.0f},
        {R"({"type":"lap","session_time":91,"lap_num":2,"current_lap_ms":0,"last_lap_ms":90000,"lap_distance_m":0})", 91.0f},
        {R"({"type":"telemetry","session_time":92,"speed_kph":220})", 92.0f},
    }, &error));
    assert(flashback.checkpoint(&error));
    assert(flashback.rewind(50.0f, &error));
    assert(flashback.append({
        {R"({"type":"lap","session_time":55,"lap_num":2,"current_lap_ms":0,"last_lap_ms":54000,"lap_distance_m":0})", 55.0f},
        {R"({"type":"telemetry","session_time":56,"speed_kph":180})", 56.0f},
    }, &error));
    assert(flashback.finish(&error));
    tnrp::detail::TnrdV5Archive flashbackRead;
    assert(flashbackRead.open(flashbackPath.string(), loadedHeader, &error));
    completeRows.clear();
    assert(flashbackRead.rowsForRange(0, 100, 0xFFFFFFFFu, completeRows, &error));
    assert(std::any_of(completeRows.begin(), completeRows.end(), [](const auto& r) {
        return r.json.find("\"session_time\":0.5") != std::string::npos;
    }));
    assert(std::any_of(completeRows.begin(), completeRows.end(), [](const auto& r) {
        return r.json.find("\"speed_kph\":180") != std::string::npos;
    }));
    assert(std::none_of(completeRows.begin(), completeRows.end(), [](const auto& r) {
        return r.json.find("\"speed_kph\":220") != std::string::npos;
    }));
    flashbackRead.close();

    // Reusing a finalized writer starts a completely independent container.
    const fs::path reusedPath = longDir / "reused_writer.tnrd";
    assert(flashback.open(reusedPath.string(), header, &error));
    assert(flashback.append({{R"({"type":"lap","session_time":3,"lap_num":3,"current_lap_ms":0,"last_lap_ms":0,"lap_distance_m":0})", 3.0f}}, &error));
    assert(flashback.finish(&error));
    tnrp::detail::TnrdV5Archive reusedRead;
    assert(reusedRead.open(reusedPath.string(), loadedHeader, &error));
    assert(reusedRead.laps().size() == 1 && reusedRead.laps().front().lapNumber == 3);
    reusedRead.close();

    // A torn fixed-header commit is recovered by scanning backward for the
    // latest valid 32-byte checkpoint footer.
    const fs::path recoveryPath = longDir / "recover_from_torn_header.tnrd";
#ifdef _WIN32
    fs::copy_file(incrementalFsPath,
                  fs::path(tnrp::detail::windowsExtendedPath(recoveryPath.string())),
                  fs::copy_options::overwrite_existing);
    std::fstream recovery(fs::path(tnrp::detail::windowsExtendedPath(recoveryPath.string())),
                          std::ios::in | std::ios::out | std::ios::binary);
#else
    fs::copy_file(incrementalFsPath,recoveryPath,fs::copy_options::overwrite_existing);
    std::fstream recovery(recoveryPath,std::ios::in | std::ios::out | std::ios::binary);
#endif
    recovery.seekp(88);char damagedCrc[4]{1,2,3,4};recovery.write(damagedCrc,4);recovery.close();
    tnrp::detail::TnrdV5Archive recovered;
    assert(recovered.open(recoveryPath.string(),loadedHeader,&error));
    assert(recovered.decompressedChunkCount()==0);
    recovered.close();

    // A byte flip in the first chunk payload must fail its CRC/frame check.
    {
#ifdef _WIN32
        std::fstream f(fs::path(tnrp::detail::windowsExtendedPath(path.string())),
                       std::ios::in | std::ios::out | std::ios::binary);
#else
        std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
#endif
        f.seekg(static_cast<std::streamoff>(firstPayloadOffset));
        char byte{};
        f.read(&byte, 1);
        byte ^= 0x20;
        f.seekp(-1, std::ios::cur);
        f.write(&byte, 1);
    }
    tnrp::detail::TnrdV5Archive corrupted;
    assert(corrupted.open(path.string(), loadedHeader, &error));
    std::barrier corruptTogether(9);
    std::array<std::thread, 8> corruptReaders;
    std::array<bool, 8> corruptFailed{};
    std::array<std::string, 8> corruptErrors;
    for (size_t i = 0; i < corruptReaders.size(); ++i) {
        corruptReaders[i] = std::thread([&, i] {
            std::vector<tnrp::detail::V5TimedRow> corruptRows;
            corruptTogether.arrive_and_wait();
            corruptFailed[i] = !corrupted.rowsForLap(
                1, tnrp::detail::v5TypeBit(1), corruptRows, &corruptErrors[i]);
        });
    }
    corruptTogether.arrive_and_wait();
    for (auto& corruptThread : corruptReaders) corruptThread.join();
    assert(std::all_of(corruptFailed.begin(), corruptFailed.end(), [](bool failed) { return failed; }));
    assert(std::all_of(corruptErrors.begin(), corruptErrors.end(), [&](const auto& value) {
        return value == corruptErrors.front();
    }));
    assert(corrupted.decompressedChunkCount() == 1);
    corrupted.close();
    std::error_code ec;
#ifdef _WIN32
    fs::remove_all(fs::path(tnrp::detail::windowsExtendedPath(root.string())), ec);
#else
    fs::remove_all(root, ec);
#endif
    return 0;
}
