#pragma once

#include <atomic>
#include <mutex>
#include <string>

#include <tnrp/Sink.h>

// Forwards each engine row to Electron as one JSON line over a named pipe
// (Windows) / unix domain socket (Linux). Electron creates the server and passes
// its address via --pipe; this connects as the client.
//
// onRow() is called from the engine's UDP and playback threads, so writes are
// mutex-guarded. Rows are written whole-line-or-not-at-all to keep the stream
// parseable; under backpressure a row is dropped rather than blocking the
// engine's hot path (POSIX). A write failure marks the sink disconnected, which
// the main loop treats as "parent gone, shut down".
class PipeSink : public tnrp::Sink {
public:
    ~PipeSink() override;

    bool connectTo(const std::string& pipePath);
    void onRow(const nlohmann::json& row) override;
    bool connected() const { return connected_.load(); }

private:
    std::mutex        mu_;
    std::atomic<bool> connected_{false};
#ifdef _WIN32
    void* handle_ = nullptr;   // HANDLE
#else
    int   fd_ = -1;
    bool  writable();
#endif
};
