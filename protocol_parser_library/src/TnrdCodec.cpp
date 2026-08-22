#include "TnrdCodec.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <vector>

#include <zlib.h>
#include <zstd.h>

#ifdef _WIN32
#  include <windows.h>
#endif

namespace tnrp::detail {
namespace {

#ifdef _WIN32
std::wstring utf8ToWide(const std::string& path) {
    if (path.empty()) return {};
    const int len = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, path.data(), static_cast<int>(path.size()), nullptr, 0);
    if (len <= 0) { errno = EINVAL; return {}; }
    std::wstring wide(static_cast<size_t>(len), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.data(),
                            static_cast<int>(path.size()), wide.data(), len) != len) {
        errno = EINVAL;
        return {};
    }
    return wide;
}
#endif

} // namespace

#ifdef _WIN32
std::wstring windowsExtendedPath(const std::string& path) {
    std::wstring wide = utf8ToWide(path);
    if (wide.empty()) return {};
    std::replace(wide.begin(), wide.end(), L'/', L'\\');

    // Device/extended paths are already in the namespace expected by Win32.
    if (wide.rfind(L"\\\\?\\", 0) == 0 || wide.rfind(L"\\\\.\\", 0) == 0)
        return wide;

    // Extended paths do not resolve relative components. GetFullPathNameW
    // makes every input absolute and normalizes '.'/'..' before prefixing it.
    const DWORD needed = GetFullPathNameW(wide.c_str(), 0, nullptr, nullptr);
    if (needed == 0) { errno = EINVAL; return {}; }
    std::wstring absolute(static_cast<size_t>(needed), L'\0');
    const DWORD written = GetFullPathNameW(
        wide.c_str(), needed, absolute.data(), nullptr);
    if (written == 0 || written >= needed) { errno = EINVAL; return {}; }
    absolute.resize(static_cast<size_t>(written));

    if (absolute.rfind(L"\\\\", 0) == 0)
        return L"\\\\?\\UNC\\" + absolute.substr(2);
    return L"\\\\?\\" + absolute;
}
#endif

namespace {

std::FILE* openFile(const std::string& path, const char* mode) {
#ifdef _WIN32
    const std::wstring wide = windowsExtendedPath(path);
    if (wide.empty()) return nullptr;
    std::wstring wmode;
    while (*mode) wmode.push_back(static_cast<wchar_t>(*mode++));
    return _wfopen(wide.c_str(), wmode.c_str());
#else
    return std::fopen(path.c_str(), mode);
#endif
}

gzFile openGzip(const std::string& path, const char* mode) {
#ifdef _WIN32
    const std::wstring wide = windowsExtendedPath(path);
    return wide.empty() ? nullptr : gzopen_w(wide.c_str(), mode);
#else
    return gzopen(path.c_str(), mode);
#endif
}

void setError(std::string* out, const std::string& message) {
    if (out) *out = message;
}

bool decompressGzip(const std::string& srcPath, std::FILE* output,
                    bool* partialOut, std::string* errorOut) {
    gzFile input = openGzip(srcPath, "rb");
    if (!input) {
        setError(errorOut, "cannot open gzip input");
        return false;
    }
    std::array<char, 131072> buffer{};
    unsigned long long written = 0;
    bool writeFailed = false;
    int n = 0;
    while ((n = gzread(input, buffer.data(), static_cast<unsigned int>(buffer.size()))) > 0) {
        if (std::fwrite(buffer.data(), 1, static_cast<size_t>(n), output) != static_cast<size_t>(n)) {
            writeFailed = true;
            break;
        }
        written += static_cast<unsigned long long>(n);
    }
    bool partial = n < 0;
    if (partialOut) *partialOut = partial;
    if (writeFailed) setError(errorOut, "write failed while decompressing gzip input");
    else if (partial && written == 0) setError(errorOut, "gzip input is corrupt and yielded no data");
    gzclose(input);
    return !writeFailed && (!partial || written > 0);
}

bool decompressZstd(const std::string& srcPath, std::FILE* output,
                    bool* partialOut, std::string* errorOut) {
    std::FILE* input = openFile(srcPath, "rb");
    if (!input) {
        setError(errorOut, "cannot open Zstandard input");
        return false;
    }
    ZSTD_DCtx* ctx = ZSTD_createDCtx();
    if (!ctx) {
        std::fclose(input);
        setError(errorOut, "cannot allocate Zstandard decompression context");
        return false;
    }

    std::vector<char> inBuffer(ZSTD_DStreamInSize());
    std::vector<char> outBuffer(ZSTD_DStreamOutSize());
    unsigned long long written = 0;
    size_t lastResult = 0;
    bool failed = false;
    bool ioFailed = false;
    bool sawInput = false;

    while (!failed) {
        const size_t read = std::fread(inBuffer.data(), 1, inBuffer.size(), input);
        if (read == 0) break;
        sawInput = true;
        ZSTD_inBuffer in{inBuffer.data(), read, 0};
        while (in.pos < in.size) {
            ZSTD_outBuffer out{outBuffer.data(), outBuffer.size(), 0};
            lastResult = ZSTD_decompressStream(ctx, &out, &in);
            if (ZSTD_isError(lastResult)) {
                setError(errorOut, std::string("Zstandard decompression failed: ") +
                                   ZSTD_getErrorName(lastResult));
                failed = true;
                break;
            }
            if (out.pos > 0) {
                if (std::fwrite(out.dst, 1, out.pos, output) != out.pos) {
                    setError(errorOut, "write failed while decompressing Zstandard input");
                    failed = true;
                    ioFailed = true;
                    break;
                }
                written += static_cast<unsigned long long>(out.pos);
            }
        }
    }
    if (std::ferror(input)) {
        setError(errorOut, "read failed while decompressing Zstandard input");
        failed = true;
        ioFailed = true;
    }

    const bool partial = failed || (sawInput && lastResult != 0);
    if (partialOut) *partialOut = partial;
    ZSTD_freeDCtx(ctx);
    std::fclose(input);
    return !ioFailed && ((!failed && !partial) || written > 0);
}

TnrdFormat detectZstdVersion(const std::string& path, std::string* errorOut) {
    std::FILE* input = openFile(path, "rb");
    if (!input) {
        setError(errorOut, "cannot reopen Zstandard recording for header detection");
        return TnrdFormat::Unknown;
    }
    ZSTD_DCtx* ctx = ZSTD_createDCtx();
    if (!ctx) {
        std::fclose(input);
        setError(errorOut, "cannot allocate Zstandard header decoder");
        return TnrdFormat::Unknown;
    }

    std::vector<char> inputBuffer(ZSTD_DStreamInSize());
    std::vector<char> outputBuffer(ZSTD_DStreamOutSize());
    std::string header;
    bool failed = false;
    while (header.find('\n') == std::string::npos && header.size() <= 64 * 1024) {
        const size_t read = std::fread(inputBuffer.data(), 1, inputBuffer.size(), input);
        if (read == 0) break;
        ZSTD_inBuffer in{inputBuffer.data(), read, 0};
        while (in.pos < in.size && header.find('\n') == std::string::npos) {
            ZSTD_outBuffer out{outputBuffer.data(), outputBuffer.size(), 0};
            const size_t rc = ZSTD_decompressStream(ctx, &out, &in);
            if (ZSTD_isError(rc)) {
                setError(errorOut, std::string("Zstandard header decompression failed: ") +
                                   ZSTD_getErrorName(rc));
                failed = true;
                break;
            }
            header.append(outputBuffer.data(), out.pos);
        }
        if (failed) break;
    }
    ZSTD_freeDCtx(ctx);
    std::fclose(input);

    if (failed) return TnrdFormat::Unknown;
    const size_t newline = header.find('\n');
    if (newline == std::string::npos) {
        setError(errorOut, "Zstandard recording has no complete JSON header");
        return TnrdFormat::Unknown;
    }
    header.resize(newline);
    const size_t key = header.find("\"magic\"");
    const size_t colon = key == std::string::npos ? key : header.find(':', key + 7);
    const size_t quote = colon == std::string::npos ? colon : header.find('"', colon + 1);
    const size_t endQuote = quote == std::string::npos ? quote : header.find('"', quote + 1);
    const std::string_view magic = endQuote == std::string::npos
        ? std::string_view{}
        : std::string_view(header).substr(quote + 1, endQuote - quote - 1);
    if (magic == "TNRD_V2") return TnrdFormat::ZstdV2;
    if (magic == "TNRD_V3") return TnrdFormat::ZstdV3;
    setError(errorOut, "Zstandard recording header is not TNRD_V2 or TNRD_V3");
    return TnrdFormat::Unknown;
}

} // namespace

std::FILE* openTnrdFile(const std::string& path, const char* mode) {
    return openFile(path, mode);
}

TnrdFormat detectTnrdFormat(const std::string& path, std::string* errorOut) {
    std::FILE* file = openFile(path, "rb");
    if (!file) {
        const int openError = errno;
        setError(errorOut, std::string("Cannot open the recording file: ") +
            (openError ? std::strerror(openError) : "unknown file-system error"));
        return TnrdFormat::Unknown;
    }
    unsigned char magic[8]{};
    const size_t read = std::fread(magic, 1, sizeof(magic), file);
    std::fclose(file);
    if (read >= 2 && magic[0] == 0x1f && magic[1] == 0x8b) return TnrdFormat::GzipV1;
    if (read == 8 && std::memcmp(magic, "TNRD_V4\0", 8) == 0) return TnrdFormat::ChunkedV4;
    if (read >= 4 && magic[0] == 0x28 && magic[1] == 0xb5 &&
        magic[2] == 0x2f && magic[3] == 0xfd) return detectZstdVersion(path, errorOut);
    setError(errorOut, "unknown TNRD compression signature");
    return TnrdFormat::Unknown;
}

bool decompressTnrd(const std::string& srcPath, const std::string& destPath,
                    TnrdFormat format, bool* partialOut, std::string* errorOut) {
    if (partialOut) *partialOut = false;
    std::FILE* output = openFile(destPath, "wb");
    if (!output) {
        setError(errorOut, "cannot create decompression temp file");
        return false;
    }
    bool ok = false;
    if (format == TnrdFormat::GzipV1)
        ok = decompressGzip(srcPath, output, partialOut, errorOut);
    else if (isZstd(format))
        ok = decompressZstd(srcPath, output, partialOut, errorOut);
    else
        setError(errorOut, "cannot decompress unknown TNRD format");
    if (std::fclose(output) != 0 && ok) {
        setError(errorOut, "failed to close decompression temp file");
        ok = false;
    }
    return ok;
}

} // namespace tnrp::detail
