#include "TNRD_V1.h"

#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <utility>

#include <zlib.h>

#include <glaze/glaze.hpp>

#include "TnrdCodec.h"

namespace tnrp::detail::TNRD_V1 {
namespace {
constexpr glz::opts kPartialRead{.null_terminated = false, .error_on_unknown_keys = false};

void discard(LoadResult& result) {
    std::error_code ec;
    if (!result.tempPath.empty()) std::filesystem::remove(result.tempPath, ec);
    result.tempPath.clear();
}

gzFile openGzipWriter(const std::string& path, const char* mode) {
#ifdef _WIN32
    const std::wstring wide = windowsExtendedPath(path);
    return wide.empty() ? nullptr : gzopen_w(wide.c_str(), mode);
#else
    return gzopen(path.c_str(), mode);
#endif
}

class GzipWriter final : public TnrdOutputStream {
public:
    GzipWriter(const std::string& path, bool append)
        : file_(openGzipWriter(path, append ? "ab" : "wb")) {
        if (!file_) {
            const int openError = errno;
            error_ = std::string("cannot open TNRD V1 gzip output") +
                (openError ? std::string(": ") + std::strerror(openError) : std::string{});
        }
    }
    ~GzipWriter() override { (void)finish(); }
    bool valid() const { return file_ != nullptr; }
    bool write(std::string_view data) override {
        if (!file_ || finished_) return fail("TNRD V1 gzip stream is not open");
        size_t pos = 0;
        while (pos < data.size()) {
            const size_t remaining = data.size() - pos;
            const unsigned int chunk = static_cast<unsigned int>(
                remaining > 0x7fffffffu ? 0x7fffffffu : remaining);
            const int written = gzwrite(file_, data.data() + pos, chunk);
            if (written <= 0) return failGzip("TNRD V1 gzwrite failed");
            pos += static_cast<size_t>(written);
        }
        return true;
    }
    bool flushRecoverable() override {
        if (!file_ || finished_) return fail("TNRD V1 gzip stream is not open");
        return gzflush(file_, Z_SYNC_FLUSH) == Z_OK || failGzip("TNRD V1 gzflush failed");
    }
    bool finish() override {
        if (finished_) return error_.empty();
        finished_ = true;
        if (!file_) return error_.empty();
        const int rc = gzclose(file_);
        file_ = nullptr;
        if (rc != Z_OK) return fail("TNRD V1 gzclose failed");
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
    gzFile file_{};
    bool finished_{};
    std::string error_;
};
} // namespace

bool load(const std::string& path, LoadResult& result, std::string& error) {
    result = LoadResult{};
    result.tempPath = (std::filesystem::temp_directory_path() /
        ("tracknrace_temp_v1_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()) + ".tmp")).string();
    if (!decompressTnrd(path, result.tempPath, TnrdFormat::GzipV1,
                        &result.partial, &error)) {
        if (error.empty()) error = "The TNRD V1 recording could not be decompressed.";
        discard(result);
        return false;
    }
    std::FILE* file = openTnrdFile(result.tempPath, "rb");
    if (!file) {
        error = "The decompressed TNRD V1 file could not be opened.";
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
        error = "The TNRD V1 header is missing or contains invalid JSON.";
        discard(result);
        return false;
    }
    if (result.header.magic != "TNRD_V1" ||
        (result.header.compression && *result.header.compression != "gzip")) {
        error = "The TNRD V1 header does not match its gzip container.";
        discard(result);
        return false;
    }
    return true;
}

void prepareHeader(HeaderRow& header) {
    header.magic = "TNRD_V1";
    header.compression.reset();
    header.track_length_m.reset();
}

std::unique_ptr<TnrdOutputStream> openWriter(const std::string& path, bool append,
                                             std::string& error) {
    auto writer = std::make_unique<GzipWriter>(path, append);
    if (writer->valid()) return writer;
    error = writer->error();
    return nullptr;
}

} // namespace tnrp::detail::TNRD_V1
