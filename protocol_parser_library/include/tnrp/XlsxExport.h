#pragma once

#include <functional>
#include <string>

namespace tnrp {

// Opens srcTnrdPath in a private, throwaway TnrdReader (independent of any
// TnrdReader/Engine that may already be in playback mode elsewhere) and
// exports every indexed row to a multi-sheet XLSX workbook: one sheet per
// row type present in the file (first-seen order), every row written as raw
// data (no aggregation), plus a small "Info" sheet built from the file
// header. On failure, *errorOut (if non-null) is set to a human-readable
// reason. onProgress (if set) is called periodically, from whatever thread
// this function runs on, with (rowsDone, totalUnits, stage) — where `stage` is
// a human-readable description of the current phase (e.g. "Writing Motion
// sheet", "Writing file to disk").
using XlsxProgressFn = std::function<void(size_t rowsDone, size_t totalUnits, const std::string& stage)>;

bool exportTnrdFileToXlsx(const std::string& srcTnrdPath,
                           const std::string& destXlsxPath,
                           std::string* errorOut = nullptr,
                           const XlsxProgressFn& onProgress = nullptr);

} // namespace tnrp
