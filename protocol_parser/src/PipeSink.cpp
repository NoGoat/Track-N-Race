#include "PipeSink.h"

#ifdef _WIN32
#  include <windows.h>
#else
#  include <sys/socket.h>
#  include <sys/un.h>
#  include <unistd.h>
#  include <cstring>
#  include <cerrno>
#endif

PipeSink::~PipeSink() {
    stop_.store(true);
    cv_.notify_all();
    if (writerThread_.joinable()) writerThread_.join();

#ifdef _WIN32
    if (handle_) CloseHandle((HANDLE)handle_);
#else
    if (fd_ >= 0) ::close(fd_);
#endif
}

void PipeSink::onRow(const std::string& json) {
    if (!connected_.load()) return;

    // Extract type for backpressure bypass (critical rows always enqueue)
    static const char KEY[] = "\"type\":\"";
    static constexpr int KLEN = sizeof(KEY) - 1;
    std::string type;
    auto pos = json.find(KEY);
    if (pos != std::string::npos) {
        pos += KLEN;
        auto end = json.find('"', pos);
        if (end != std::string::npos) type = json.substr(pos, end - pos);
    }
    bool force = (type == "playback_loaded" || type == "playback_lap_blocks" ||
                  type == "playback_state"  || type == "protocol_status");

    std::string line = json;
    line.push_back('\n');

    {
        std::unique_lock<std::mutex> lk(mu_);
        if (!force && queue_.size() > 2000) return;
        queue_.push(std::move(line));
    }
    cv_.notify_one();
}

void PipeSink::writerLoop() {
    while (!stop_.load()) {
        std::string line;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [this] { return stop_.load() || !queue_.empty(); });
            if (stop_.load() && queue_.empty()) break;
            line = std::move(queue_.front());
            queue_.pop();
        }

        if (!connected_.load()) continue;

        const char* p = line.data();
        size_t left = line.size();
        while (left > 0) {
#ifdef _WIN32
            DWORD wrote = 0;
            if (!WriteFile((HANDLE)handle_, p, (DWORD)left, &wrote, nullptr) || wrote == 0) {
                connected_.store(false);
                break;
            }
            p += wrote; left -= wrote;
#else
            ssize_t n = ::send(fd_, p, left, MSG_NOSIGNAL);
            if (n > 0) { p += n; left -= (size_t)n; continue; }
            if (n < 0 && (errno == EINTR)) continue;
            connected_.store(false);
            break;
#endif
        }
    }
}

#ifdef _WIN32
bool PipeSink::connectTo(const std::string& pipePath) {
    for (int attempt = 0; attempt < 50; ++attempt) {
        HANDLE h = CreateFileA(pipePath.c_str(), GENERIC_WRITE, 0, nullptr,
                               OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            handle_ = h;
            connected_.store(true);
            writerThread_ = std::thread(&PipeSink::writerLoop, this);
            return true;
        }
        if (GetLastError() != ERROR_PIPE_BUSY && GetLastError() != ERROR_FILE_NOT_FOUND)
            break;
        Sleep(100);
    }
    return false;
}
#else
bool PipeSink::connectTo(const std::string& pipePath) {
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, pipePath.c_str(), sizeof(addr.sun_path) - 1);

    for (int attempt = 0; attempt < 50; ++attempt) {
        if (::connect(fd, (sockaddr*)&addr, sizeof(addr)) == 0) {
            fd_ = fd;
            connected_.store(true);
            writerThread_ = std::thread(&PipeSink::writerLoop, this);
            return true;
        }
        usleep(100 * 1000);
    }
    ::close(fd);
    return false;
}
#endif
