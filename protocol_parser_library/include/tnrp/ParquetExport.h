#pragma once

#include <functional>
#include <string>

namespace tnrp {

// Progress as a fraction of the whole export in [0, 1], plus a short stage
// description ("Writing Speed", ...). Called from the exporting thread.
using ParquetProgressFn = std::function<void(double fraction, const std::string& stage)>;
// Polled between rows; returning true cancels the export.
using ParquetCancelFn = std::function<bool()>;

// Opens srcTnrdPath in a private, throwaway TnrdReader (independent of any
// reader already playing it) and exports every stored row as Parquet tables,
// one <Table>.parquet per table, into a new folder inside destParentDir named
// "<recording>_parquet", or "<recording>_parquet (2)", "(3)", ... when that
// name is taken. An existing folder is never touched.
//
// The tables are written into a hidden staging folder in destParentDir first;
// it is renamed to the first free name only once every table is complete, and
// removed on failure or cancellation. Paths are UTF-8. On success *outFolder
// (if non-null) is the new folder's path; on failure *errorOut (if non-null)
// says why ("cancelled" when cancelled).
bool exportTnrdFileToParquet(const std::string& srcTnrdPath,
                             const std::string& destParentDir,
                             std::string* outFolder = nullptr,
                             std::string* errorOut = nullptr,
                             const ParquetProgressFn& onProgress = nullptr,
                             const ParquetCancelFn& cancelled = nullptr);

} // namespace tnrp
