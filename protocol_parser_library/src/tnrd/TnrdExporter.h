#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

#include "tnrp/control_rows.h"

// Parquet export, one module per TNRD generation (TNRD_V1_Exporter.cpp ...
// TNRD_V6_Exporter.cpp). Each exporter knows how its generation stores rows
// and turns them into Parquet tables through tnrp/ParquetWriter.h; the
// modules share no code with each other. This header only declares the calls.
//
// A reader's exportParquet() hands its stored rows to its generation's
// exporter, which writes one <Table>.parquet per table into
// request.directory (an existing, empty staging folder). Installing the
// finished folder is the caller's job (src/ParquetExport.cpp).
namespace tnrp::detail {

class TnrdV6Archive;

struct ParquetExportRequest {
    std::string directory;  // UTF-8
    HeaderRow header;
    // fraction in [0, 1] of the whole export, plus a short stage description.
    std::function<void(double fraction, const std::string& stage)> progress;
    // Polled between rows; true stops the export (it then fails with "cancelled").
    std::function<bool()> cancelled;
};

// A generation's stored JSONL rows in storage order. forEach visits every row
// once per call (exporters make more than one pass) with its session time, or
// NaN when the row itself carries it. The visitor returns false to stop; then
// forEach returns false too, as it does on a read error (with errorOut set).
struct JsonlExportSource {
    using Visitor = std::function<bool(float sessionTime, std::string_view line)>;
    size_t rowCount{};
    std::function<bool(const Visitor&, std::string* errorOut)> forEach;
};

bool exportTnrdV1Parquet(const JsonlExportSource& rows, const ParquetExportRequest& request,
                         std::string* errorOut);
bool exportTnrdV2Parquet(const JsonlExportSource& rows, const ParquetExportRequest& request,
                         std::string* errorOut);
bool exportTnrdV3Parquet(const JsonlExportSource& rows, const ParquetExportRequest& request,
                         std::string* errorOut);
bool exportTnrdV4Parquet(const JsonlExportSource& rows, const ParquetExportRequest& request,
                         std::string* errorOut);
bool exportTnrdV5Parquet(const JsonlExportSource& rows, const ParquetExportRequest& request,
                         std::string* errorOut);
bool exportTnrdV6Parquet(TnrdV6Archive& archive, const ParquetExportRequest& request,
                         std::string* errorOut);

} // namespace tnrp::detail
