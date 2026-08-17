#include "TnrdV4.h"
#include <tnrp/TnrdReader.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

int main() {
    namespace fs = std::filesystem;
    const fs::path path = fs::temp_directory_path() /
        ("tnrd_v4_boundary_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()) + ".tnrd");
    tnrp::HeaderRow header;
    header.protocol = 2026;
    header.track_id = 7;
    header.track_name = "Boundary Test";
    header.track_length_m = 5000;
    header.session_type = 10;
    std::string error;
    const std::vector<tnrp::detail::V4SourceRow> rows = {
        {R"({"type":"lap","session_time":1,"lap_num":1,"current_lap_ms":0,"last_lap_ms":0})", 1.0f},
        {R"({"type":"telemetry","session_time":90.9,"speed_kph":200})", 90.9f},
        // First packet for lap 2 arrives 16 ms after its true boundary.
        {R"({"type":"lap","session_time":91.016,"lap_num":2,"current_lap_ms":16,"last_lap_ms":90000})", 91.016f},
        // Opening replay without FLBK can briefly emit a slightly older lap
        // row. It is below the writer's 200 ms jitter threshold and must not
        // move the V4 lap index backwards.
        {R"({"type":"lap","session_time":91.032,"lap_num":1,"current_lap_ms":90032,"last_lap_ms":0})", 91.032f},
    };
    if (!tnrp::detail::writeTnrdV4(path.string(), header, rows, &error)) return 1;
    tnrp::detail::TnrdV4Archive archive;
    tnrp::HeaderRow loaded;
    if (!archive.open(path.string(), loaded, &error)) return 2;
    const auto& laps = archive.laps();
    if (laps.size() != 2) return 3;
    if (std::fabs(laps[0].endSessionTime - laps[1].startSessionTime) > 0.0001f) return 4;
    archive.close();

    const fs::path practicePath = fs::temp_directory_path() /
        ("tnrd_v4_practice_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()) + ".tnrd");
    header.session_type = 4;
    const std::vector<tnrp::detail::V4SourceRow> practiceRows = {
        {R"({"type":"lap","session_time":10,"lap_num":1,"current_lap_ms":0,"last_lap_ms":0,"driver_status":0})", 10.0f},
        {R"({"type":"lap","session_time":30,"lap_num":1,"current_lap_ms":0,"last_lap_ms":0,"driver_status":3})", 30.0f},
        {R"({"type":"lap","session_time":100,"lap_num":1,"current_lap_ms":0,"last_lap_ms":0,"driver_status":1})", 100.0f},
        {R"({"type":"telemetry","session_time":105,"speed_kph":200})", 105.0f},
        {R"({"type":"lap","session_time":200,"lap_num":1,"current_lap_ms":100000,"last_lap_ms":0,"driver_status":2})", 200.0f},
        {R"({"type":"lap","session_time":300,"lap_num":1,"current_lap_ms":0,"last_lap_ms":0,"driver_status":1})", 300.0f},
        {R"({"type":"telemetry","session_time":310,"speed_kph":210})", 310.0f},
        {R"({"type":"lap","session_time":400,"lap_num":2,"current_lap_ms":0,"last_lap_ms":100000,"driver_status":1})", 400.0f},
    };
    if (!tnrp::detail::writeTnrdV4(practicePath.string(), header, practiceRows, &error)) return 5;
    tnrp::HeaderRow practiceHeader;
    tnrp::TnrdReader reader;
    if (!reader.load(practicePath.string(), practiceHeader)) return 6;
    const std::string lapData = reader.getLapDataMessage(1, tnrp::detail::v4TypeBit(1));
    if (lapData.find("\"startSessionTime\":300") == std::string::npos) return 7;
    if (lapData.find("\"session_time\":105") != std::string::npos) return 8;
    if (lapData.find("\"session_time\":310") == std::string::npos) return 9;
    reader.close();
    std::error_code ignored;
    fs::remove(path, ignored);
    fs::remove(practicePath, ignored);
    return 0;
}
