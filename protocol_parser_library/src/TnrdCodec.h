#pragma once

#include <cstdio>
#include <memory>
#include <string>
#include <string_view>

#include "tnrp/TnrdFormat.h"

namespace tnrp::detail {

// Opens a UTF-8 path. Windows callers transparently use the extended-length
// namespace so recording and playback are not constrained by MAX_PATH.
std::FILE* openTnrdFile(const std::string& path, const char* mode);

#ifdef _WIN32
// Converts a UTF-8 path to an absolute Windows extended-length path. Keep the
// original UTF-8 path for UI/error reporting; use this only at filesystem API
// boundaries so paths beyond MAX_PATH work without changing recorded names.
std::wstring windowsExtendedPath(const std::string& path);
#endif

class TnrdOutputStream {
public:
    virtual ~TnrdOutputStream() = default;
    virtual bool write(std::string_view data) = 0;
    virtual bool flushRecoverable() = 0;
    virtual bool finish() = 0;
    virtual const std::string& error() const = 0;
};

TnrdFormat detectTnrdFormat(const std::string& path, std::string* errorOut = nullptr);

// Decompresses one or more concatenated frames/members into a plain JSONL
// file. A truncated tail is accepted when at least some bytes were recovered;
// TnrdReader will discard the unfinished final JSONL line while indexing.
bool decompressTnrd(const std::string& srcPath, const std::string& destPath,
                    TnrdFormat format, bool* partialOut = nullptr,
                    std::string* errorOut = nullptr);

} // namespace tnrp::detail
