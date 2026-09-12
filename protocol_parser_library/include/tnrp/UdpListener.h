#pragma once

#include <cstdint>
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "tnrp/Config.h"

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
    bool start(uint16_t port, const std::string& bindAddress, Handler handler,
               const std::vector<UdpForwardTarget>& forwardTargets = {});
    void stop();

    bool isRunning() const { return running_.load(); }
    std::string lastError() const;

private:
    std::atomic<bool> running_{false};
    std::thread       thread_;
    mutable std::mutex errorMutex_;
    std::string       lastError_;
    // Winsock SOCKET is pointer-sized. Keeping it in an int truncates the
    // handle on 64-bit Windows and can leak the bound socket on restart.
    std::uintptr_t    rawSock_{static_cast<std::uintptr_t>(-1)};

    void runLoop(uint16_t port, std::string bindAddress, Handler handler,
                 std::vector<UdpForwardTarget> forwardTargets);
    void setLastError(std::string error);
};

} // namespace tnrp
