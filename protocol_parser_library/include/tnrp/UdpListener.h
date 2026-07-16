#pragma once

#include <cstdint>
#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace tnrp {

// Cross-platform UDP receiver. Dual-mode via TNRP_USE_QT:
//   - TNRP_USE_QT defined  -> QUdpSocket (for linking into the native Qt app),
//   - otherwise            -> raw winsock2 / POSIX sockets (for the headless bridge).
// Both run a dedicated receive thread and invoke `handler` for each datagram.
// The public header stays Qt-free so consumers don't need Qt headers to use it.
//
// The handler is called on the receive thread; consumers must be thread-safe.
class UdpListener {
public:
    using Handler = std::function<void(const uint8_t* data, int length)>;

    UdpListener() = default;
    ~UdpListener();

    // Bind `port` on `bindAddress` and begin receiving. Returns false on bind
    // failure (see lastError()). Calling start() again restarts on the new port.
    bool start(uint16_t port, const std::string& bindAddress, Handler handler);
    void stop();

    bool isRunning() const { return running_.load(); }
    std::string lastError() const { return lastError_; }

private:
    std::atomic<bool> running_{false};
    std::thread       thread_;
    std::string       lastError_;
    int               rawSock_{-1};

    void runLoop(uint16_t port, std::string bindAddress, Handler handler);
};

} // namespace tnrp
