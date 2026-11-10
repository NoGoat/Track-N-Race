#include "tnrp/ParquetExport.h"

#include "tnrp/TnrdReader.h"
#include "TnrdCodec.h"
#include "tnrd/TnrdReaderBackend.h"

#include <chrono>
#include <filesystem>
#include <system_error>

#ifdef _WIN32
#  include <windows.h>
#endif

// Recording -> folder of Parquet tables. The per-generation exporters
// (src/tnrd/TNRD_V*_Exporter.cpp) write the tables; this file loads the
// recording, stages the folder and installs it under a free name.
namespace tnrp {

namespace fs = std::filesystem;

namespace {

// A filesystem path for a UTF-8 string; extended-length on Windows so deep
// destinations work like the rest of the library's file access.
fs::path nativePath(const std::string& utf8) {
#ifdef _WIN32
    return fs::path(detail::windowsExtendedPath(utf8));
#else
    return fs::path(utf8);
#endif
}

std::string joinUtf8(const std::string& directory, const std::string& name) {
    if (directory.empty()) return name;
    const char last = directory.back();
    return (last == '/' || last == '\\') ? directory + name : directory + "/" + name;
}

// The recording's file name without its extension, as UTF-8.
std::string recordingName(const std::string& srcPath) {
    size_t start = srcPath.find_last_of("/\\");
    start = start == std::string::npos ? 0 : start + 1;
    std::string name = srcPath.substr(start);
    const size_t dot = name.find_last_of('.');
    if (dot != std::string::npos && dot > 0) name.resize(dot);
    return name.empty() ? "recording" : name;
}

void removeTree(const std::string& utf8) {
    std::error_code ignored;
    fs::remove_all(nativePath(utf8), ignored);
}

} // namespace

bool TnrdReader::exportParquet(const HeaderRow& header, const std::string& directory, std::string* errorOut,
                               const std::function<void(double, const std::string&)>& onProgress,
                               const std::function<bool()>& cancelled) {
    if (!isLoaded() || !backend_) {
        if (errorOut) *errorOut = "no .tnrd loaded";
        return false;
    }
    detail::ParquetExportRequest request;
    request.directory = directory;
    request.header = header;
    request.progress = onProgress;
    request.cancelled = cancelled;
    return backend_->exportParquet(request, errorOut);
}

bool exportTnrdFileToParquet(const std::string& srcTnrdPath, const std::string& destParentDir,
                             std::string* outFolder, std::string* errorOut,
                             const ParquetProgressFn& onProgress, const ParquetCancelFn& cancelled) {
    std::error_code ec;
    if (!fs::is_directory(nativePath(destParentDir), ec)) {
        if (errorOut) *errorOut = "the destination is not a folder";
        return false;
    }

    TnrdReader reader;
    HeaderRow header;
    if (!reader.load(srcTnrdPath, header)) {
        if (errorOut) *errorOut = "failed to open/decompress/parse .tnrd file";
        return false;
    }

    const std::string baseName = recordingName(srcTnrdPath) + "_parquet";
    const std::string staging = joinUtf8(destParentDir,
        "." + baseName + ".tmp-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    if (!fs::create_directory(nativePath(staging), ec)) {
        if (errorOut) *errorOut = "could not create a folder in the destination";
        return false;
    }
#ifdef _WIN32
    const std::wstring stagingWide = detail::windowsExtendedPath(staging);
    const DWORD attributes = GetFileAttributesW(stagingWide.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES)
        SetFileAttributesW(stagingWide.c_str(), attributes | FILE_ATTRIBUTE_HIDDEN);
#endif

    std::string exportError;
    const bool exported = reader.exportParquet(header, staging, &exportError, onProgress, cancelled);
    reader.close();
    if (!exported) {
        removeTree(staging);
        if (errorOut) *errorOut = exportError.empty() ? "export failed" : exportError;
        return false;
    }

#ifdef _WIN32
    const DWORD finished = GetFileAttributesW(stagingWide.c_str());
    if (finished != INVALID_FILE_ATTRIBUTES)
        SetFileAttributesW(stagingWide.c_str(), finished & ~DWORD(FILE_ATTRIBUTE_HIDDEN));
#endif

    // Install under the first free name. rename() fails rather than replace
    // an existing folder, so a folder that appeared meanwhile is skipped too.
    for (int number = 1; number < 10000; ++number) {
        const std::string name = number == 1 ? baseName : baseName + " (" + std::to_string(number) + ")";
        const std::string target = joinUtf8(destParentDir, name);
        if (fs::exists(nativePath(target), ec)) continue;
        fs::rename(nativePath(staging), nativePath(target), ec);
        if (!ec) {
            if (outFolder) *outFolder = target;
            return true;
        }
        if (!fs::exists(nativePath(target))) break;  // failed for another reason
    }
    removeTree(staging);
    if (errorOut) *errorOut = "could not move the finished export into place" +
                              (ec ? " (" + ec.message() + ")" : std::string());
    return false;
}

} // namespace tnrp
