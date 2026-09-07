#include "tnrp/PairDiscovery.h"

#include <chrono>
#include <cstring>
#include <mutex>
#include <sstream>
#include <utility>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using Socket = SOCKET;
static constexpr Socket kInvalidSocket = INVALID_SOCKET;
static void closeSocket(Socket socket) { closesocket(socket); }
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
using Socket = int;
static constexpr Socket kInvalidSocket = -1;
static void closeSocket(Socket socket) { close(socket); }
#endif

namespace tnrp {
namespace {

constexpr const char* kGroup = "239.255.84.78";
constexpr uint16_t kPort = 20778;
constexpr const char* kPrefix = "TNRPAIR1";

#ifdef _WIN32
struct WinsockGuard {
    WinsockGuard() { WSADATA data{}; ok = WSAStartup(MAKEWORD(2, 2), &data) == 0; }
    ~WinsockGuard() { if (ok) WSACleanup(); }
    bool ok = false;
};
#endif

bool validToken(const std::string& value, size_t maxLength) {
    if (value.empty() || value.size() > maxLength) return false;
    for (unsigned char ch : value) {
        if (ch < 0x20 || ch == '|' || ch == '\n' || ch == '\r') return false;
    }
    return true;
}

std::string beacon(const PairServiceInfo& info) {
    std::ostringstream out;
    out << kPrefix << '|' << info.serverId << '|' << info.name << '|'
        << info.port << '|' << (info.pairing ? '1' : '0');
    return out.str();
}

bool parseBeacon(const char* data, size_t size, const std::string& address,
                 DiscoveredPairService& result) {
    std::string value(data, size);
    std::string parts[5];
    size_t start = 0;
    for (size_t i = 0; i < 5; ++i) {
        const size_t end = value.find('|', start);
        if (i < 4 && end == std::string::npos) return false;
        parts[i] = value.substr(start, i == 4 ? std::string::npos : end - start);
        start = end + 1;
    }
    if (parts[0] != kPrefix || !validToken(parts[1], 128) ||
        !validToken(parts[2], 96)) return false;
    try {
        const unsigned long parsedPort = std::stoul(parts[3]);
        if (parsedPort == 0 || parsedPort > 65535 ||
            (parts[4] != "0" && parts[4] != "1")) return false;
        result = {parts[1], parts[2], address,
                  static_cast<uint16_t>(parsedPort), parts[4] == "1"};
        return true;
    } catch (...) {
        return false;
    }
}

void setReceiveTimeout(Socket socket) {
#ifdef _WIN32
    DWORD timeout = 500;
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
    timeval timeout{0, 500000};
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif
}

} // namespace

struct PairDiscoveryAdvertiser::Impl {
    std::mutex mutex;
    PairServiceInfo info;
    Socket socket = kInvalidSocket;
#ifdef _WIN32
    std::unique_ptr<WinsockGuard> winsock;
#endif
};

PairDiscoveryAdvertiser::PairDiscoveryAdvertiser() : impl_(std::make_unique<Impl>()) {}
PairDiscoveryAdvertiser::~PairDiscoveryAdvertiser() { stop(); }

bool PairDiscoveryAdvertiser::start(PairServiceInfo info, std::string* error) {
    stop();
    if (!validToken(info.serverId, 128) || !validToken(info.name, 96) || info.port == 0) {
        if (error) *error = "Invalid paired-service discovery information";
        return false;
    }
#ifdef _WIN32
    impl_->winsock = std::make_unique<WinsockGuard>();
    if (!impl_->winsock->ok) {
        if (error) *error = "Unable to initialize Winsock for discovery";
        impl_->winsock.reset();
        return false;
    }
#endif
    impl_->socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (impl_->socket == kInvalidSocket) {
        if (error) *error = "Unable to create discovery socket";
        return false;
    }
    unsigned char ttl = 1;
    setsockopt(impl_->socket, IPPROTO_IP, IP_MULTICAST_TTL,
               reinterpret_cast<const char*>(&ttl), sizeof(ttl));
    {
        std::lock_guard lock(impl_->mutex);
        impl_->info = std::move(info);
    }
    running_.store(true);
    thread_ = std::thread([this] {
        sockaddr_in destination{};
        destination.sin_family = AF_INET;
        destination.sin_port = htons(kPort);
        inet_pton(AF_INET, kGroup, &destination.sin_addr);
        while (running_.load()) {
            PairServiceInfo current;
            {
                std::lock_guard lock(impl_->mutex);
                current = impl_->info;
            }
            const std::string message = beacon(current);
            sendto(impl_->socket, message.data(), static_cast<int>(message.size()), 0,
                   reinterpret_cast<sockaddr*>(&destination), sizeof(destination));
            for (int i = 0; i < 10 && running_.load(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    });
    return true;
}

void PairDiscoveryAdvertiser::update(PairServiceInfo info) {
    if (!validToken(info.serverId, 128) || !validToken(info.name, 96) || info.port == 0) return;
    std::lock_guard lock(impl_->mutex);
    impl_->info = std::move(info);
}

void PairDiscoveryAdvertiser::stop() {
    running_.store(false);
    if (thread_.joinable()) thread_.join();
    if (impl_->socket != kInvalidSocket) {
        closeSocket(impl_->socket);
        impl_->socket = kInvalidSocket;
    }
#ifdef _WIN32
    impl_->winsock.reset();
#endif
}

struct PairDiscoveryBrowser::Impl {
    Callback callback;
    Socket socket = kInvalidSocket;
#ifdef _WIN32
    std::unique_ptr<WinsockGuard> winsock;
#endif
};

PairDiscoveryBrowser::PairDiscoveryBrowser() : impl_(std::make_unique<Impl>()) {}
PairDiscoveryBrowser::~PairDiscoveryBrowser() { stop(); }

bool PairDiscoveryBrowser::start(Callback callback, std::string* error) {
    stop();
    if (!callback) {
        if (error) *error = "A discovery callback is required";
        return false;
    }
#ifdef _WIN32
    impl_->winsock = std::make_unique<WinsockGuard>();
    if (!impl_->winsock->ok) {
        if (error) *error = "Unable to initialize Winsock for discovery";
        impl_->winsock.reset();
        return false;
    }
#endif
    impl_->socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (impl_->socket == kInvalidSocket) {
        if (error) *error = "Unable to create discovery socket";
        return false;
    }
    int reuse = 1;
    setsockopt(impl_->socket, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(kPort);
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(impl_->socket, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
        if (error) *error = "Unable to bind discovery socket";
        stop();
        return false;
    }
    ip_mreq membership{};
    inet_pton(AF_INET, kGroup, &membership.imr_multiaddr);
    membership.imr_interface.s_addr = htonl(INADDR_ANY);
    if (setsockopt(impl_->socket, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                   reinterpret_cast<const char*>(&membership), sizeof(membership)) != 0) {
        if (error) *error = "Unable to join discovery multicast group";
        stop();
        return false;
    }
    setReceiveTimeout(impl_->socket);
    impl_->callback = std::move(callback);
    running_.store(true);
    thread_ = std::thread([this] {
        char buffer[512];
        while (running_.load()) {
            sockaddr_in sender{};
#ifdef _WIN32
            int senderSize = sizeof(sender);
#else
            socklen_t senderSize = sizeof(sender);
#endif
            const int count = recvfrom(impl_->socket, buffer, sizeof(buffer), 0,
                reinterpret_cast<sockaddr*>(&sender), &senderSize);
            if (count <= 0) continue;
            char address[INET_ADDRSTRLEN]{};
            if (!inet_ntop(AF_INET, &sender.sin_addr, address, sizeof(address))) continue;
            DiscoveredPairService service;
            if (parseBeacon(buffer, static_cast<size_t>(count), address, service))
                impl_->callback(service);
        }
    });
    return true;
}

void PairDiscoveryBrowser::stop() {
    running_.store(false);
    if (thread_.joinable()) thread_.join();
    if (impl_->socket != kInvalidSocket) {
        closeSocket(impl_->socket);
        impl_->socket = kInvalidSocket;
    }
    impl_->callback = {};
#ifdef _WIN32
    impl_->winsock.reset();
#endif
}

} // namespace tnrp
