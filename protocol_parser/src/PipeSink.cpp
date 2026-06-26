#include "PipeSink.h"

#ifdef _WIN32
#  include <windows.h>
#else
#  include <sys/socket.h>
#  include <sys/un.h>
#  include <sys/select.h>
#  include <unistd.h>
#  include <cstring>
#  include <cerrno>
#endif

PipeSink::~PipeSink() {
#ifdef _WIN32
    if (handle_) CloseHandle((HANDLE)handle_);
#else
    if (fd_ >= 0) ::close(fd_);
#endif
}

#ifdef _WIN32

bool PipeSink::connectTo(const std::string& pipePath) {
    // Electron passes a name like \\.\pipe\tnrp-<id>. Retry briefly in case the
    // server isn't listening yet.
    for (int attempt = 0; attempt < 50; ++attempt) {
        HANDLE h = CreateFileA(pipePath.c_str(), GENERIC_WRITE, 0, nullptr,
                               OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            handle_ = h;
            connected_.store(true);
            return true;
        }
        if (GetLastError() != ERROR_PIPE_BUSY && GetLastError() != ERROR_FILE_NOT_FOUND)
            break;
        Sleep(100);
    }
    return false;
}

void PipeSink::onRow(const nlohmann::json& row) {
    if (!connected_.load()) return;
    std::string line = row.dump();
    line.push_back('\n');
    std::lock_guard<std::mutex> lk(mu_);
    const char* p = line.data();
    size_t left = line.size();
    while (left > 0) {
        DWORD wrote = 0;
        if (!WriteFile((HANDLE)handle_, p, (DWORD)left, &wrote, nullptr) || wrote == 0) {
            connected_.store(false);   // parent gone / pipe broken
            return;
        }
        p += wrote; left -= wrote;
    }
}

#else  // POSIX

bool PipeSink::connectTo(const std::string& pipePath) {
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, pipePath.c_str(), sizeof(addr.sun_path) - 1);

    // Retry briefly: Electron may still be setting up its listening server.
    for (int attempt = 0; attempt < 50; ++attempt) {
        if (::connect(fd, (sockaddr*)&addr, sizeof(addr)) == 0) {
            fd_ = fd;
            connected_.store(true);
            return true;
        }
        usleep(100 * 1000);
    }
    ::close(fd);
    return false;
}

bool PipeSink::writable() {
    fd_set wf;
    FD_ZERO(&wf);
    FD_SET(fd_, &wf);
    timeval tv{}; tv.tv_sec = 0; tv.tv_usec = 0;   // poll, don't block
    return ::select(fd_ + 1, nullptr, &wf, nullptr, &tv) > 0;
}

void PipeSink::onRow(const nlohmann::json& row) {
    if (!connected_.load()) return;
    std::string line = row.dump();
    line.push_back('\n');

    std::lock_guard<std::mutex> lk(mu_);
    // Backpressure: if the socket can't accept data right now, drop this whole
    // row rather than block the engine's hot path or write a partial (corrupt) line.
    if (!writable()) return;

    const char* p = line.data();
    size_t left = line.size();
    while (left > 0) {
        ssize_t n = ::send(fd_, p, left, MSG_NOSIGNAL);
        if (n > 0) { p += n; left -= (size_t)n; continue; }
        if (n < 0 && (errno == EINTR)) continue;
        connected_.store(false);   // EPIPE / ECONNRESET: parent gone
        return;
    }
}

#endif
