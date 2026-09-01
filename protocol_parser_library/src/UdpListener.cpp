#include "tnrp/UdpListener.h"

#include <algorithm>
#include <cstdio>
#include <utility>
#include <vector>

#define TRACE(msg) do { fprintf(stderr, "[native] " msg "\n"); fflush(stderr); } while (0)

#ifdef TNRP_USE_QT
// ── Qt path (for linking into the native Qt app) ─────────────────────────────
#include <QUdpSocket>
#include <QHostAddress>

namespace tnrp {

UdpListener::~UdpListener() { stop(); }

bool UdpListener::start(uint16_t port, const std::string& bindAddress, Handler handler,
                        const std::vector<UdpForwardTarget>& forwardTargets) {
    stop();
    lastError_.clear();
    running_.store(true);
    // Probe-bind on the calling thread so start() can report failure synchronously.
    {
        QUdpSocket probe;
        if (!probe.bind(QHostAddress(QString::fromStdString(bindAddress)), port,
                        QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
            lastError_ = probe.errorString().toStdString();
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
                lastError_ = "Invalid UDP forwarding destination: " + target.address +
                    ":" + std::to_string(target.port);
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
#else
using SOCKET = int;
static constexpr int INVALID_SOCKET = -1;
static void closeSock(int s) { ::close(s); }
#endif

UdpListener::~UdpListener() { stop(); }

bool UdpListener::start(uint16_t port, const std::string& bindAddress, Handler handler,
                        const std::vector<UdpForwardTarget>& forwardTargets) {
    TRACE("UdpListener::start: entry");
    stop();
    lastError_.clear();
    TRACE("UdpListener::start: stop() done");
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        lastError_ = "WSAStartup failed";
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
            lastError_ = "Invalid UDP forwarding destination: " + target.address +
                ":" + std::to_string(target.port);
#ifdef _WIN32
            WSACleanup();
#endif
            return false;
        }
        destinations.push_back(dest);
    }
    SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        lastError_ = "socket() failed";
#ifdef _WIN32
        WSACleanup();
#endif
        return false;
    }
    TRACE("UdpListener::start: socket() created");

    int reuse = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
    int broadcast = 1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char*)&broadcast, sizeof(broadcast));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (bindAddress.empty() || bindAddress == "0.0.0.0") {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (::inet_pton(AF_INET, bindAddress.c_str(), &addr.sin_addr) != 1) {
        lastError_ = "Invalid IPv4 bind address: " + bindAddress;
        closeSock(s);
#ifdef _WIN32
        WSACleanup();
#endif
        return false;
    }

    if (::bind(s, (sockaddr*)&addr, sizeof(addr)) != 0) {
        lastError_ = "bind() failed for " + bindAddress + ":" + std::to_string(port);
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
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
#else
    timeval tv{}; tv.tv_sec = 0; tv.tv_usec = 200 * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

    rawSock_ = s;
    running_.store(true);
    TRACE("UdpListener::start: spawning receive thread");
    // Pass the bound socket to the loop via a tiny heap handoff (int fits in the
    // bindAddress slot we already have — reuse runLoop signature for clarity).
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
            }
            // n <= 0 is a timeout or transient error; loop and re-check running_.
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
    if (rawSock_ != -1) {
        // To guarantee recvfrom unblocks on Linux, we send a dummy packet to ourselves.
        // sockaddr_in we bound to might be INADDR_ANY, but 127.0.0.1 will reach it.
        sockaddr_in name{};
        socklen_t namelen = sizeof(name);
        if (getsockname(rawSock_, (sockaddr*)&name, &namelen) == 0) {
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
        
        closeSock(rawSock_);
        rawSock_ = -1;
#ifdef _WIN32
        WSACleanup();
#endif
    }
    if (thread_.joinable()) thread_.join();
}

} // namespace tnrp

#endif // TNRP_USE_QT
