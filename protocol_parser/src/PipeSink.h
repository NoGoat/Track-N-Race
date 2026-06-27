#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <queue>
#include <thread>
#include <condition_variable>

#include <tnrp/Sink.h>

// Forwards each engine row to Electron as one JSON line over a named pipe
// (Windows) / unix domain socket (Linux). Rows arrive pre-serialised, so this
// class just appends '\n' and queues them for the writer thread.
//
// onRow() is called from the engine's UDP and playback threads; writes are
// mutex-guarded. Under backpressure a row is dropped rather than blocking the
// engine's hot path. A write failure marks the sink disconnected.
class PipeSink : public tnrp::Sink {
public:
    ~PipeSink() override;

    bool connectTo(const std::string& pipePath);
    void onRow(const std::string& json) override;
    bool connected() const { return connected_.load(); }

private:
    void writerLoop();

    std::mutex              mu_;
    std::condition_variable cv_;
    std::queue<std::string> queue_;
    std::thread             writerThread_;
    std::atomic<bool>       stop_{false};
    std::atomic<bool>       connected_{false};
#ifdef _WIN32
    void* handle_ = nullptr;
#else
    int   fd_ = -1;
#endif
};
