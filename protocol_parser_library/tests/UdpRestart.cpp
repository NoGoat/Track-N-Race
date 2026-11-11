// Engine::restartUdp rebinds the listener in place: the old port is released,
// the new one receives, forwarding follows the new destinations, and neither a
// restart nor the engine's destruction trips over datagrams still arriving.
// The checks below have side effects, so they stay on in release builds.
#undef NDEBUG
#include "tnrp/Engine.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
using socklen_t = int;
static void closeSock(SOCKET s) { closesocket(s); }
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <sys/time.h>
#  include <unistd.h>
using SOCKET = int;
static constexpr int INVALID_SOCKET = -1;
static void closeSock(int s) { ::close(s); }
#endif

namespace {

struct NullSink : tnrp::Sink {
    void onRow(const std::string&) override {}
};

sockaddr_in loopback(uint16_t port) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return addr;
}

// A UDP socket bound to `port` on loopback (0 picks a free one), or INVALID_SOCKET.
SOCKET boundSocket(uint16_t port) {
    SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    assert(s != INVALID_SOCKET);
    const sockaddr_in addr = loopback(port);
    if (::bind(s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        closeSock(s);
        return INVALID_SOCKET;
    }
    return s;
}

uint16_t portOf(SOCKET s) {
    sockaddr_in name{};
    socklen_t length = sizeof(name);
    assert(::getsockname(s, reinterpret_cast<sockaddr*>(&name), &length) == 0);
    return ntohs(name.sin_port);
}

uint16_t freePort() {
    const SOCKET s = boundSocket(0);
    assert(s != INVALID_SOCKET);
    const uint16_t port = portOf(s);
    closeSock(s);
    return port;
}

void sendTo(SOCKET s, uint16_t port) {
    const char payload[4] = {};
    const sockaddr_in addr = loopback(port);
    ::sendto(s, payload, sizeof(payload), 0, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
}

// Sends to `port` until the engine has counted a datagram past `before`.
bool receives(tnrp::Engine& engine, SOCKET sender, uint16_t port, uint64_t before) {
    for (int attempt = 0; attempt < 200; ++attempt) {
        sendTo(sender, port);
        if (engine.datagramsReceived() > before) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

} // namespace

int main() {
#ifdef _WIN32
    WSADATA wsa;
    assert(WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
#endif
    const uint16_t portA = freePort();
    const uint16_t portB = freePort();
    assert(portA != portB);
    const SOCKET sender = boundSocket(0);
    assert(sender != INVALID_SOCKET);

    NullSink sink;
    tnrp::Config config;
    config.port = portA;
    config.bindAddress = "127.0.0.1";
    auto engine = std::make_unique<tnrp::Engine>(config, &sink);
    assert(engine->startUdp());
    assert(receives(*engine, sender, portA, engine->datagramsReceived()));

    // The new port receives and the old one is free again.
    assert(engine->restartUdp(portB, "127.0.0.1", {}));
    assert(receives(*engine, sender, portB, engine->datagramsReceived()));
    const SOCKET reclaimed = boundSocket(portA);
    assert(reclaimed != INVALID_SOCKET);
    closeSock(reclaimed);

    // Forwarding follows the destinations given to the restart.
    const SOCKET forwarded = boundSocket(0);
    assert(forwarded != INVALID_SOCKET);
#ifdef _WIN32
    DWORD timeout = 2000;
    setsockopt(forwarded, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
    timeval timeout{2, 0};
    setsockopt(forwarded, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif
    assert(engine->restartUdp(portB, "127.0.0.1", {{"127.0.0.1", portOf(forwarded)}}));
    assert(receives(*engine, sender, portB, engine->datagramsReceived()));
    char buffer[16];
    assert(::recv(forwarded, buffer, sizeof(buffer), 0) == 4);
    closeSock(forwarded);

    // A destination that loops back to the listener is refused, and the
    // engine can be bound again afterwards.
    assert(!engine->restartUdp(portB, "127.0.0.1", {{"127.0.0.1", portB}}));
    assert(!engine->udpLastError().empty());
    assert(engine->restartUdp(portB, "127.0.0.1", {}));

    // Restarts, then the engine's destruction, while datagrams keep arriving.
    std::atomic<bool> flooding{true};
    std::thread flood([&] {
        const SOCKET s = boundSocket(0);
        while (flooding.load()) {
            sendTo(s, portA);
            sendTo(s, portB);
        }
        closeSock(s);
    });
    for (int round = 0; round < 10; ++round)
        assert(engine->restartUdp(round % 2 ? portB : portA, "127.0.0.1", {}));
    engine->stopUdp();
    assert(engine->startUdp());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    engine.reset();
    flooding.store(false);
    flood.join();

    closeSock(sender);
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
