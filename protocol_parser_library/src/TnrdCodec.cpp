#include "TnrdCodec.h"

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
    const int len = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    if (len <= 0) return {};
    std::wstring wide(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wide.data(), len);
    return wide;
}
#endif

std::FILE* openFile(const std::string& path, const char* mode) {
#ifdef _WIN32
    const std::wstring wide = utf8ToWide(path);
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
    const std::wstring wide = utf8ToWide(path);
    return wide.empty() ? nullptr : gzopen_w(wide.c_str(), mode);
#else
    return gzopen(path.c_str(), mode);
#endif
}

void setError(std::string* out, const std::string& message) {
    if (out) *out = message;
}

class GzipOutputStream final : public TnrdOutputStream {
public:
    GzipOutputStream(const std::string& path, bool append)
        : file_(openGzip(path, append ? "ab" : "wb")) {
        if (!file_) error_ = "cannot open gzip output";
    }

    ~GzipOutputStream() override { (void)finish(); }

    bool valid() const { return file_ != nullptr; }

    bool write(std::string_view data) override {
        if (!file_ || finished_) return fail("gzip stream is not open");
        size_t pos = 0;
        while (pos < data.size()) {
            const size_t remaining = data.size() - pos;
            const unsigned int chunk = static_cast<unsigned int>(
                remaining > 0x7fffffffu ? 0x7fffffffu : remaining);
            const int written = gzwrite(file_, data.data() + pos, chunk);
            if (written <= 0) return failGzip("gzwrite failed");
            pos += static_cast<size_t>(written);
        }
        return true;
    }

    bool flushRecoverable() override {
        if (!file_ || finished_) return fail("gzip stream is not open");
        if (gzflush(file_, Z_SYNC_FLUSH) != Z_OK) return failGzip("gzflush failed");
        return true;
    }

    bool finish() override {
        if (finished_) return error_.empty();
        finished_ = true;
        if (!file_) return error_.empty();
        const int rc = gzclose(file_);
        file_ = nullptr;
        if (rc != Z_OK) return fail("gzclose failed");
        return error_.empty();
    }

    const std::string& error() const override { return error_; }

private:
    bool fail(const std::string& message) {
        if (error_.empty()) error_ = message;
        return false;
    }

    bool failGzip(const char* prefix) {
        int code = Z_OK;
        const char* detail = file_ ? gzerror(file_, &code) : nullptr;
        return fail(std::string(prefix) + (detail ? std::string(": ") + detail : std::string{}));
    }

    gzFile file_ = nullptr;
    bool finished_ = false;
    std::string error_;
};

class ZstdOutputStream final : public TnrdOutputStream {
public:
    ZstdOutputStream(const std::string& path, bool append)
        : file_(openFile(path, append ? "ab" : "wb")), ctx_(ZSTD_createCCtx()),
          output_(ZSTD_CStreamOutSize()) {
        if (!file_) error_ = "cannot open Zstandard output";
        else if (!ctx_) error_ = "cannot allocate Zstandard compression context";
        else {
            if (!check(ZSTD_CCtx_setParameter(ctx_, ZSTD_c_compressionLevel, 3),
                       "cannot set Zstandard compression level")) return;
            (void)check(ZSTD_CCtx_setParameter(ctx_, ZSTD_c_checksumFlag, 1),
                        "cannot enable Zstandard checksum");
        }
    }

    ~ZstdOutputStream() override {
        (void)finish();
        if (ctx_) ZSTD_freeCCtx(ctx_);
    }

    bool valid() const { return file_ && ctx_ && error_.empty(); }

    bool write(std::string_view data) override {
        if (!valid() || finished_) return fail("Zstandard stream is not open");
        ZSTD_inBuffer input{data.data(), data.size(), 0};
        while (input.pos < input.size) {
            if (!pump(input, ZSTD_e_continue, false)) return false;
        }
        return true;
    }

    bool flushRecoverable() override {
        if (!valid() || finished_) return fail("Zstandard stream is not open");
        ZSTD_inBuffer input{nullptr, 0, 0};
        size_t remaining = 1;
        while (remaining != 0) {
            if (!pump(input, ZSTD_e_flush, true, &remaining)) return false;
        }
        if (std::fflush(file_) != 0) return fail("fflush failed for Zstandard output");
        return true;
    }

    bool finish() override {
        if (finished_) return error_.empty();
        finished_ = true;
        bool ok = error_.empty();
        if (file_ && ctx_ && ok) {
            ZSTD_inBuffer input{nullptr, 0, 0};
            size_t remaining = 1;
            while (remaining != 0) {
                if (!pump(input, ZSTD_e_end, true, &remaining)) { ok = false; break; }
            }
        }
        if (file_) {
            if (std::fclose(file_) != 0 && ok) ok = fail("fclose failed for Zstandard output");
            file_ = nullptr;
        }
        return ok && error_.empty();
    }

    const std::string& error() const override { return error_; }

private:
    bool check(size_t result, const char* prefix) {
        if (!ZSTD_isError(result)) return true;
        return fail(std::string(prefix) + ": " + ZSTD_getErrorName(result));
    }

    bool pump(ZSTD_inBuffer& input, ZSTD_EndDirective directive, bool allowEmpty,
              size_t* remainingOut = nullptr) {
        ZSTD_outBuffer output{output_.data(), output_.size(), 0};
        const size_t remaining = ZSTD_compressStream2(ctx_, &output, &input, directive);
        if (!check(remaining, "Zstandard compression failed")) return false;
        if (output.pos > 0 && std::fwrite(output.dst, 1, output.pos, file_) != output.pos)
            return fail("write failed for Zstandard output");
        if (!allowEmpty && output.pos == 0 && input.pos == input.size && remaining != 0)
            return fail("Zstandard compression made no progress");
        if (remainingOut) *remainingOut = remaining;
        return true;
    }

    bool fail(const std::string& message) {
        if (error_.empty()) error_ = message;
        return false;
    }

    std::FILE* file_ = nullptr;
    ZSTD_CCtx* ctx_ = nullptr;
    std::vector<char> output_;
    bool finished_ = false;
    std::string error_;
};

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

} // namespace

TnrdFormat detectTnrdFormat(const std::string& path, std::string* errorOut) {
    std::FILE* file = openFile(path, "rb");
    if (!file) {
        setError(errorOut, "cannot open TNRD file");
        return TnrdFormat::Unknown;
    }
    unsigned char magic[4]{};
    const size_t read = std::fread(magic, 1, sizeof(magic), file);
    std::fclose(file);
    if (read >= 2 && magic[0] == 0x1f && magic[1] == 0x8b) return TnrdFormat::GzipV1;
    if (read == 4 && magic[0] == 0x28 && magic[1] == 0xb5 &&
        magic[2] == 0x2f && magic[3] == 0xfd) return TnrdFormat::ZstdV2;
    setError(errorOut, "unknown TNRD compression signature");
    return TnrdFormat::Unknown;
}

std::unique_ptr<TnrdOutputStream> openTnrdOutput(
    const std::string& path, TnrdFormat format, bool append, std::string* errorOut) {
    if (format == TnrdFormat::GzipV1) {
        auto stream = std::make_unique<GzipOutputStream>(path, append);
        if (stream->valid()) return stream;
        setError(errorOut, stream->error());
        return nullptr;
    }
    if (format == TnrdFormat::ZstdV2) {
        auto stream = std::make_unique<ZstdOutputStream>(path, append);
        if (stream->valid()) return stream;
        setError(errorOut, stream->error());
        return nullptr;
    }
    setError(errorOut, "cannot open output for unknown TNRD format");
    return nullptr;
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
    else if (format == TnrdFormat::ZstdV2)
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
