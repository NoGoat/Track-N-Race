#include "tnrp/UdpListener.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

#define TRACE(msg) do { fprintf(stderr, "[native] " msg "\n"); fflush(stderr); } while (0)

namespace tnrp {

std::string UdpListener::lastError() const {
    std::lock_guard<std::mutex> lock(errorMutex_);
    return lastError_;
}

void UdpListener::setLastError(std::string error) {
    std::lock_guard<std::mutex> lock(errorMutex_);
    lastError_ = std::move(error);
}

} // namespace tnrp

#ifdef TNRP_USE_QT
// ── Qt path (for linking into the native Qt app) ─────────────────────────────
#include <QUdpSocket>
#include <QHostAddress>

namespace tnrp {

UdpListener::~UdpListener() { stop(); }

bool UdpListener::start(uint16_t port, const std::string& bindAddress, Handler handler,
                        const std::vector<UdpForwardTarget>& forwardTargets) {
    stop();
    setLastError({});
    running_.store(true);
    // Probe-bind on the calling thread so start() can report failure synchronously.
    {
        QUdpSocket probe;
        if (!probe.bind(QHostAddress(QString::fromStdString(bindAddress)), port,
                        QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
            setLastError(probe.errorString().toStdString());
            running_.store(false);
            return false;
        }
        const size_t count = std::min(forwardTargets.size(), kMaxUdpForwardTargets);
        for (size_t i = 0; i < count; ++i) {
            const auto& target = forwardTargets[i];
            QHostAddress address;
            if (target.port == 0 ||
                (target.port == port && (target.address.rfind("127.", 0) == 0 ||
                                         target.address == bindAddress)) ||
                !address.setAddress(QString::fromStdString(target.address)) ||
                address.protocol() != QAbstractSocket::IPv4Protocol) {
                setLastError("Invalid UDP forwarding destination: " + target.address +
                    ":" + std::to_string(target.port));
                running_.store(false);
                return false;
            }
        }
    }
    const size_t count = std::min(forwardTargets.size(), kMaxUdpForwardTargets);
    thread_ = std::thread(&UdpListener::runLoop, this, port, bindAddress, std::move(handler),
                          std::vector<UdpForwardTarget>(forwardTargets.begin(),
                                                       forwardTargets.begin() + count));
    return true;
}

void UdpListener::runLoop(uint16_t port, std::string bindAddress, Handler handler,
                          std::vector<UdpForwardTarget> forwardTargets) {
    // The socket is created and used entirely on this worker thread.
    QUdpSocket sock;
    if (!sock.bind(QHostAddress(QString::fromStdString(bindAddress)), port,
                   QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
        running_.store(false);
        return;
    }
    std::vector<char> buf;
    std::vector<std::pair<QHostAddress, quint16>> destinations;
    destinations.reserve(forwardTargets.size());
    for (const auto& target : forwardTargets)
        destinations.emplace_back(QHostAddress(QString::fromStdString(target.address)), target.port);
    while (running_.load()) {
        if (!sock.waitForReadyRead(200)) continue;   // timeout: re-check running_
        while (sock.hasPendingDatagrams()) {
            qint64 sz = sock.pendingDatagramSize();
            if (sz <= 0) { sock.readDatagram(nullptr, 0); continue; }
            buf.resize((size_t)sz);
            qint64 n = sock.readDatagram(buf.data(), sz);
            if (!running_.load()) break;
            if (n > 0) {
                for (const auto& [address, targetPort] : destinations)
                    sock.writeDatagram(buf.data(), n, address, targetPort);
                if (handler) handler(reinterpret_cast<const uint8_t*>(buf.data()), (int)n);
            }
        }
    }
}

void UdpListener::stop() {
    running_.store(false);
    if (thread_.joinable()) thread_.join();
}

} // namespace tnrp

#else
// ── Raw socket path (for the headless bridge) ────────────────────────────────
#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  pragma comment(lib, "ws2_32.lib")
   using socklen_t = int;
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
#  include <cerrno>
#  include <cstring>
#endif

namespace tnrp {

#ifdef _WIN32
static void closeSock(SOCKET s) { closesocket(s); }
static int socketErrorCode() { return WSAGetLastError(); }
static std::string socketErrorText(const char* operation, int code) {
    return std::string(operation) + " failed (Winsock error " + std::to_string(code) + ")";
}
#else
using SOCKET = int;
static constexpr int INVALID_SOCKET = -1;
static void closeSock(int s) { ::close(s); }
static int socketErrorCode() { return errno; }
static std::string socketErrorText(const char* operation, int code) {
    return std::string(operation) + " failed (" + std::strerror(code) + ", errno " +
        std::to_string(code) + ")";
}
#endif

static constexpr std::uintptr_t INVALID_RAW_SOCKET = static_cast<std::uintptr_t>(-1);

UdpListener::~UdpListener() { stop(); }

bool UdpListener::start(uint16_t port, const std::string& bindAddress, Handler handler,
                        const std::vector<UdpForwardTarget>& forwardTargets) {
    TRACE("UdpListener::start: entry");
    stop();
    setLastError({});
    TRACE("UdpListener::start: stop() done");
#ifdef _WIN32
    WSADATA wsa;
    const int startupError = WSAStartup(MAKEWORD(2, 2), &wsa);
    if (startupError != 0) {
        setLastError(socketErrorText("WSAStartup", startupError));
        return false;
    }
    TRACE("UdpListener::start: WSAStartup done");
#endif
    const size_t forwardCount = std::min(forwardTargets.size(), kMaxUdpForwardTargets);
    std::vector<sockaddr_in> destinations;
    destinations.reserve(forwardCount);
    for (size_t i = 0; i < forwardCount; ++i) {
        const auto& target = forwardTargets[i];
        sockaddr_in dest{};
        dest.sin_family = AF_INET;
        dest.sin_port = htons(target.port);
        if (target.port == 0 ||
            (target.port == port && (target.address.rfind("127.", 0) == 0 ||
                                     target.address == bindAddress)) ||
            ::inet_pton(AF_INET, target.address.c_str(), &dest.sin_addr) != 1) {
            setLastError("Invalid UDP forwarding destination: " + target.address +
                ":" + std::to_string(target.port));
#ifdef _WIN32
            WSACleanup();
#endif
            return false;
        }
        destinations.push_back(dest);
    }
    SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        setLastError(socketErrorText("socket()", socketErrorCode()));
#ifdef _WIN32
        WSACleanup();
#endif
        return false;
    }
    TRACE("UdpListener::start: socket() created");

#ifdef _WIN32
    // SO_REUSEADDR on Windows permits another process to bind the same UDP
    // endpoint and makes datagram delivery indeterminate. Demand exclusive
    // ownership so a port conflict is reported instead of silently receiving
    // no telemetry.
    int exclusive = 1;
    if (setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                   reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) != 0) {
        setLastError(socketErrorText("setsockopt(SO_EXCLUSIVEADDRUSE)", socketErrorCode()));
        closeSock(s);
        WSACleanup();
        return false;
    }
#else
    int reuse = 1;
    if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0) {
        setLastError(socketErrorText("setsockopt(SO_REUSEADDR)", socketErrorCode()));
        closeSock(s);
        return false;
    }
#endif

    if (!destinations.empty()) {
        int broadcast = 1;
        if (setsockopt(s, SOL_SOCKET, SO_BROADCAST,
                       reinterpret_cast<const char*>(&broadcast), sizeof(broadcast)) != 0) {
            setLastError(socketErrorText("setsockopt(SO_BROADCAST)", socketErrorCode()));
            closeSock(s);
#ifdef _WIN32
            WSACleanup();
#endif
            return false;
        }
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (bindAddress.empty() || bindAddress == "0.0.0.0") {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (::inet_pton(AF_INET, bindAddress.c_str(), &addr.sin_addr) != 1) {
        setLastError("Invalid IPv4 bind address: " + bindAddress);
        closeSock(s);
#ifdef _WIN32
        WSACleanup();
#endif
        return false;
    }

    if (::bind(s, (sockaddr*)&addr, sizeof(addr)) != 0) {
        const int errorCode = socketErrorCode();
        const std::string address = bindAddress.empty() ? "0.0.0.0" : bindAddress;
        setLastError("UDP bind failed for " + address + ":" + std::to_string(port) +
                     " — " + socketErrorText("bind()", errorCode));
        fprintf(stderr, "[native][udp] %s\n", lastError().c_str());
        fflush(stderr);
        closeSock(s);
#ifdef _WIN32
        WSACleanup();
#endif
        return false;
    }
    TRACE("UdpListener::start: bind() done");

    // Receive timeout so the loop can poll running_ and exit promptly on stop().
#ifdef _WIN32
    DWORD tv = 200;  // ms
    if (setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&tv), sizeof(tv)) != 0) {
        setLastError(socketErrorText("setsockopt(SO_RCVTIMEO)", socketErrorCode()));
        closeSock(s);
        WSACleanup();
        return false;
    }
#else
    timeval tv{}; tv.tv_sec = 0; tv.tv_usec = 200 * 1000;
    if (setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) {
        setLastError(socketErrorText("setsockopt(SO_RCVTIMEO)", socketErrorCode()));
        closeSock(s);
        return false;
    }
#endif

    rawSock_ = static_cast<std::uintptr_t>(s);
    running_.store(true);
    TRACE("UdpListener::start: spawning receive thread");
    // Capture the native handle directly; rawSock_ keeps the same pointer-width
    // value so stop() can close this exact socket on 64-bit Windows.
    thread_ = std::thread([this, s, handler, destinations = std::move(destinations)]() {
        uint8_t buf[65536];
        while (running_.load()) {
            sockaddr_in from{};
            socklen_t fromLen = sizeof(from);
            int n = (int)::recvfrom(s, (char*)buf, sizeof(buf), 0, (sockaddr*)&from, &fromLen);
            if (!running_.load()) break;
            if (n > 0) {
                for (const auto& dest : destinations)
                    ::sendto(s, reinterpret_cast<const char*>(buf), n, 0,
                             reinterpret_cast<const sockaddr*>(&dest), sizeof(dest));
                if (handler) handler(buf, n);
                continue;
            }
            if (n == 0) continue;

            const int errorCode = socketErrorCode();
#ifdef _WIN32
            const bool transient = errorCode == WSAETIMEDOUT || errorCode == WSAEWOULDBLOCK ||
                                   errorCode == WSAEINTR;
#else
            const bool transient = errorCode == EAGAIN || errorCode == EWOULDBLOCK ||
                                   errorCode == EINTR;
#endif
            if (transient) continue;

            const std::string error = socketErrorText("recvfrom()", errorCode);
            setLastError(error);
            fprintf(stderr, "[native][udp] %s\n", error.c_str());
            fflush(stderr);
            running_.store(false);
            break;
        }
    });
    TRACE("UdpListener::start: thread spawned, returning true");
    return true;
}

void UdpListener::runLoop(uint16_t, std::string, Handler,
                          std::vector<UdpForwardTarget>) {
    // Unused in the raw path; the receive loop is inlined in start() so it can
    // own the bound socket descriptor. Kept to satisfy the shared header.
}

void UdpListener::stop() {
    running_.store(false);
    if (rawSock_ != INVALID_RAW_SOCKET) {
        const SOCKET socket = static_cast<SOCKET>(rawSock_);
        rawSock_ = INVALID_RAW_SOCKET;
        // To guarantee recvfrom unblocks on Linux, we send a dummy packet to ourselves.
        // sockaddr_in we bound to might be INADDR_ANY, but 127.0.0.1 will reach it.
        sockaddr_in name{};
        socklen_t namelen = sizeof(name);
        if (getsockname(socket, (sockaddr*)&name, &namelen) == 0) {
            SOCKET dummy = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (dummy != INVALID_SOCKET) {
                sockaddr_in dest{};
                dest.sin_family = AF_INET;
                dest.sin_port = name.sin_port;
                dest.sin_addr.s_addr = inet_addr("127.0.0.1");
                const char msg = 0;
                ::sendto(dummy, &msg, 1, 0, (sockaddr*)&dest, sizeof(dest));
                closeSock(dummy);
            }
        }
        
        closeSock(socket);
#ifdef _WIN32
        WSACleanup();
#endif
    }
    if (thread_.joinable()) thread_.join();
}

} // namespace tnrp

#endif // TNRP_USE_QT
