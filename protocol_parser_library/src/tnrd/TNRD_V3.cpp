#include "TNRD_V3.h"

#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <utility>
#include <vector>

#include <zstd.h>

#include <glaze/glaze.hpp>

#include "TnrdCodec.h"

namespace tnrp::detail::TNRD_V3 {
namespace {
constexpr glz::opts kPartialRead{.null_terminated = false, .error_on_unknown_keys = false};

void discard(LoadResult& result) {
    std::error_code ec;
    if (!result.tempPath.empty()) std::filesystem::remove(result.tempPath, ec);
    result.tempPath.clear();
}

class ZstdWriter final : public TnrdOutputStream {
public:
    ZstdWriter(const std::string& path, bool append)
        : file_(openTnrdFile(path, append ? "ab" : "wb")) {
        if (!file_) {
            const int openError = errno;
            error_ = std::string("cannot open TNRD V3 Zstandard output") +
                (openError ? std::string(": ") + std::strerror(openError) : std::string{});
            return;
        }
        ctx_ = ZSTD_createCCtx();
        if (!ctx_) { error_ = "cannot allocate TNRD V3 compression context"; return; }
        output_.resize(ZSTD_CStreamOutSize());
        if (!check(ZSTD_CCtx_setParameter(ctx_, ZSTD_c_compressionLevel, 3),
                   "cannot set TNRD V3 compression level")) return;
        (void)check(ZSTD_CCtx_setParameter(ctx_, ZSTD_c_checksumFlag, 1),
                    "cannot enable TNRD V3 checksum");
    }
    ~ZstdWriter() override { (void)finish(); if (ctx_) ZSTD_freeCCtx(ctx_); }
    bool valid() const { return file_ && ctx_ && error_.empty(); }
    bool write(std::string_view data) override {
        if (!valid() || finished_) return fail("TNRD V3 Zstandard stream is not open");
        ZSTD_inBuffer input{data.data(), data.size(), 0};
        while (input.pos < input.size)
            if (!pump(input, ZSTD_e_continue, false)) return false;
        return true;
    }
    bool flushRecoverable() override {
        if (!valid() || finished_) return fail("TNRD V3 Zstandard stream is not open");
        ZSTD_inBuffer input{nullptr, 0, 0};
        size_t remaining = 1;
        while (remaining != 0)
            if (!pump(input, ZSTD_e_flush, true, &remaining)) return false;
        return std::fflush(file_) == 0 || fail("fflush failed for TNRD V3 output");
    }
    bool finish() override {
        if (finished_) return error_.empty();
        finished_ = true;
        bool ok = error_.empty();
        if (file_ && ctx_ && ok) {
            ZSTD_inBuffer input{nullptr, 0, 0};
            size_t remaining = 1;
            while (remaining != 0)
                if (!pump(input, ZSTD_e_end, true, &remaining)) { ok = false; break; }
        }
        if (file_) {
            if (std::fclose(file_) != 0 && ok) ok = fail("fclose failed for TNRD V3 output");
            file_ = nullptr;
        }
        return ok && error_.empty();
    }
    const std::string& error() const override { return error_; }

private:
    bool check(size_t result, const char* prefix) {
        return !ZSTD_isError(result) ||
            fail(std::string(prefix) + ": " + ZSTD_getErrorName(result));
    }
    bool pump(ZSTD_inBuffer& input, ZSTD_EndDirective directive, bool allowEmpty,
              size_t* remainingOut = nullptr) {
        ZSTD_outBuffer output{output_.data(), output_.size(), 0};
        const size_t remaining = ZSTD_compressStream2(ctx_, &output, &input, directive);
        if (!check(remaining, "TNRD V3 compression failed")) return false;
        if (output.pos > 0 && std::fwrite(output.dst, 1, output.pos, file_) != output.pos)
            return fail("write failed for TNRD V3 output");
        if (!allowEmpty && output.pos == 0 && input.pos == input.size && remaining != 0)
            return fail("TNRD V3 compression made no progress");
        if (remainingOut) *remainingOut = remaining;
        return true;
    }
    bool fail(const std::string& message) {
        if (error_.empty()) error_ = message;
        return false;
    }
    std::FILE* file_{};
    ZSTD_CCtx* ctx_{};
    std::vector<char> output_;
    bool finished_{};
    std::string error_;
};
} // namespace

bool load(const std::string& path, LoadResult& result, std::string& error) {
    result = LoadResult{};
    result.tempPath = (std::filesystem::temp_directory_path() /
        ("tracknrace_temp_v3_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()) + ".tmp")).string();
    if (!decompressTnrd(path, result.tempPath, TnrdFormat::ZstdV3,
                        &result.partial, &error)) {
        if (error.empty()) error = "The TNRD V3 recording could not be decompressed.";
        discard(result);
        return false;
    }
    std::FILE* file = openTnrdFile(result.tempPath, "rb");
    if (!file) {
        error = "The decompressed TNRD V3 file could not be opened.";
        discard(result);
        return false;
    }
    std::string line;
    char buffer[8192];
    while (std::fgets(buffer, sizeof(buffer), file)) {
        line += buffer;
        if (!line.empty() && line.back() == '\n') break;
        if (std::strlen(buffer) < sizeof(buffer) - 1) break;
    }
    std::fclose(file);
    if (glz::read<kPartialRead>(result.header, line)) {
        error = "The TNRD V3 header is missing or contains invalid JSON.";
        discard(result);
        return false;
    }
    if (result.header.magic != "TNRD_V3" || !result.header.compression ||
        *result.header.compression != "zstd") {
        error = "The TNRD V3 header does not match its Zstandard container.";
        discard(result);
        return false;
    }
    return true;
}

void prepareHeader(HeaderRow& header) {
    header.magic = "TNRD_V3";
    header.compression = "zstd";
}

std::unique_ptr<TnrdOutputStream> openWriter(const std::string& path, bool append,
                                             std::string& error) {
    auto writer = std::make_unique<ZstdWriter>(path, append);
    if (writer->valid()) return writer;
    error = writer->error();
    return nullptr;
}

} // namespace tnrp::detail::TNRD_V3
