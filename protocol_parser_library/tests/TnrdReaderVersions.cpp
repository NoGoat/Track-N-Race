#include "TnrdCodec.h"
#include "tnrd/TNRD_V1.h"
#include "tnrd/TNRD_V2.h"
#include "tnrd/TNRD_V3.h"
#include "tnrd/TNRD_V4.h"
#include "tnrp/TnrdReader.h"

#include <array>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace {

std::string magicFor(tnrp::TnrdFormat format) {
    switch (format) {
        case tnrp::TnrdFormat::GzipV1: return "TNRD_V1";
        case tnrp::TnrdFormat::ZstdV2: return "TNRD_V2";
        case tnrp::TnrdFormat::ZstdV3: return "TNRD_V3";
        case tnrp::TnrdFormat::ChunkedV4: return "TNRD_V4";
        default: return {};
    }
}

tnrp::HeaderRow headerFor(tnrp::TnrdFormat format) {
    tnrp::HeaderRow header;
    header.magic = magicFor(format);
    if (format != tnrp::TnrdFormat::GzipV1) header.compression = "zstd";
    header.protocol = 2026;
    header.track_id = 7;
    header.track_name = "Version Test Track";
    if (format == tnrp::TnrdFormat::ZstdV3 || format == tnrp::TnrdFormat::ChunkedV4)
        header.track_length_m = 5000;
    header.session_type = 10;
    header.session_name = "Race";
    header.start_time = 123456789;
    return header;
}

std::unique_ptr<tnrp::detail::TnrdOutputStream> openWriter(
        tnrp::TnrdFormat format, const std::string& path, std::string& error) {
    switch (format) {
        case tnrp::TnrdFormat::GzipV1:
            return tnrp::detail::TNRD_V1::openWriter(path, false, error);
        case tnrp::TnrdFormat::ZstdV2:
            return tnrp::detail::TNRD_V2::openWriter(path, false, error);
        case tnrp::TnrdFormat::ZstdV3:
            return tnrp::detail::TNRD_V3::openWriter(path, false, error);
        default:
            return nullptr;
    }
}

} // namespace

int main() {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() /
        ("tnrd_reader_versions_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);

    const std::vector<tnrp::detail::V4SourceRow> rows = {
        {R"({"type":"lap","session_time":1,"lap_num":1,"current_lap_ms":0,"last_lap_ms":0,"lap_distance_m":0})", 1.0f},
        {R"({"type":"session","session_time":1.1,"track_id":7,"session_type":10})", 1.1f},
        {R"({"type":"telemetry","session_time":1.2,"speed_kph":100})", 1.2f},
    };

    const std::array formats{
        tnrp::TnrdFormat::GzipV1,
        tnrp::TnrdFormat::ZstdV2,
        tnrp::TnrdFormat::ZstdV3,
        tnrp::TnrdFormat::ChunkedV4,
    };
    std::string error;
    for (const auto format : formats) {
        const fs::path path = root / (magicFor(format) + ".tnrd");
        const tnrp::HeaderRow header = headerFor(format);
        if (format == tnrp::TnrdFormat::ChunkedV4) {
            assert(tnrp::detail::writeTnrdV4(path.string(), header, rows, &error));
        } else {
            auto stream = openWriter(format, path.string(), error);
            assert(stream);
            assert(stream->write(tnrp::writeJson(header) + "\n"));
            for (const auto& row : rows) assert(stream->write(row.line + "\n"));
            assert(stream->finish());
        }

        assert(tnrp::detail::detectTnrdFormat(path.string(), &error) == format);
        tnrp::TnrdReader reader;
        tnrp::HeaderRow loadedHeader;
        assert(reader.load(path.string(), loadedHeader));
        assert(reader.loadedFormat() == format);
        assert(loadedHeader.magic == magicFor(format));
        assert(reader.readRange(0.0f, 2.0f).size() == rows.size());
        reader.close();
    }

    std::error_code ec;
    fs::remove_all(root, ec);
    return 0;
}
