#pragma once

#include <string>

namespace tnrp {

// Opens srcTnrdPath in a private, throwaway TnrdReader (independent of any
// TnrdReader/Engine that may already be in playback mode elsewhere) and
// exports every indexed row to a multi-sheet XLSX workbook: one sheet per
// row type present in the file (first-seen order), every row written as raw
// data (no aggregation), plus a small "Info" sheet built from the file
// header. On failure, *errorOut (if non-null) is set to a human-readable
// reason.
bool exportTnrdFileToXlsx(const std::string& srcTnrdPath,
                           const std::string& destXlsxPath,
                           std::string* errorOut = nullptr);

} // namespace tnrp
