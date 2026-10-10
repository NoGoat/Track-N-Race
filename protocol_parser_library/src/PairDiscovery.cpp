#include "tnrp/PairDiscovery.h"

#include <array>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <windns.h>
using Socket = SOCKET;
static constexpr Socket kInvalidSocket = INVALID_SOCKET;
static void closeSocket(Socket socket) { closesocket(socket); }
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
using Socket = int;
static constexpr Socket kInvalidSocket = -1;
static void closeSocket(Socket socket) { close(socket); }
#if defined(__APPLE__)
#include <dns_sd.h>
#endif
#endif

namespace tnrp {
namespace {

constexpr const char* kServiceType = "_tracknrace-pair._tcp";
constexpr const char* kTxtVersion = "3";
// How often the advertiser notices a changed address (DHCP renewal, Wi-Fi
// switch) and republishes.
constexpr auto kAddressCheck = std::chrono::seconds(5);

#ifdef _WIN32
struct WinsockGuard {
    WinsockGuard() { WSADATA data{}; ok = WSAStartup(MAKEWORD(2, 2), &data) == 0; }
    ~WinsockGuard() { if (ok) WSACleanup(); }
    bool ok = false;
};
#endif

bool validToken(const std::string& value, size_t maxLength) {
    if (value.empty() || value.size() > maxLength) return false;
    for (unsigned char ch : value)
        if (ch < 0x20 || ch == 0x7f) return false;
    return true;
}

// A DNS-SD instance name is one label: no dots, at most 63 bytes.
std::string instanceLabel(const std::string& name) {
    std::string label;
    for (const unsigned char ch : name) {
        if (ch < 0x20 || ch == 0x7f) continue;
        label.push_back(ch == '.' ? '-' : static_cast<char>(ch));
    }
    if (label.empty()) label = "Track N Race";
    if (label.size() > 63) label.resize(63);
    // Never cut a UTF-8 sequence in half.
    while (!label.empty() && (static_cast<unsigned char>(label.back()) & 0xc0) == 0x80)
        label.pop_back();
    if (!label.empty() && (static_cast<unsigned char>(label.back()) & 0xc0) == 0xc0)
        label.pop_back();
    return label;
}

// A host name of our own so the SRV target never collides with the machine's.
std::string hostLabel(const std::string& serverId) {
    std::string label = "tnr-";
    for (const unsigned char ch : serverId) {
        if (label.size() >= 16) break;
        if (std::isalnum(ch)) label.push_back(static_cast<char>(std::tolower(ch)));
    }
    return label;
}

} // namespace

std::string primaryIpv4Address() {
    // connect() on UDP sends nothing; it only selects the outgoing interface.
    Socket probe = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (probe != kInvalidSocket) {
        sockaddr_in remote{};
        remote.sin_family = AF_INET;
        remote.sin_port = htons(53);
        inet_pton(AF_INET, "8.8.8.8", &remote.sin_addr);
        if (connect(probe, reinterpret_cast<sockaddr*>(&remote), sizeof(remote)) == 0) {
            sockaddr_in local{};
#ifdef _WIN32
            int size = sizeof(local);
#else
            socklen_t size = sizeof(local);
#endif
            char text[INET_ADDRSTRLEN]{};
            if (getsockname(probe, reinterpret_cast<sockaddr*>(&local), &size) == 0 &&
                local.sin_addr.s_addr != htonl(INADDR_ANY) &&
                inet_ntop(AF_INET, &local.sin_addr, text, sizeof(text)) &&
                std::string_view(text).substr(0, 4) != "127.") {
                closeSocket(probe);
                return text;
            }
        }
        closeSocket(probe);
    }
    // No default route (an offline hotspot): take the first usable address.
    char host[256]{};
    if (gethostname(host, sizeof(host) - 1) != 0) return {};
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* result = nullptr;
    if (getaddrinfo(host, nullptr, &hints, &result) != 0) return {};
    std::string address;
    for (addrinfo* item = result; item; item = item->ai_next) {
        const auto* endpoint = reinterpret_cast<const sockaddr_in*>(item->ai_addr);
        char text[INET_ADDRSTRLEN]{};
        if (inet_ntop(AF_INET, &endpoint->sin_addr, text, sizeof(text)) &&
            std::string_view(text).substr(0, 4) != "127.") {
            address = text;
            break;
        }
    }
    freeaddrinfo(result);
    return address;
}

// ── Windows: the system mDNS responder (dnsapi, Windows 10 1809+) ─────────
#ifdef _WIN32
namespace {

using ConstructInstanceFn = PDNS_SERVICE_INSTANCE (WINAPI*)(
    PCWSTR, PCWSTR, PIP4_ADDRESS, PIP6_ADDRESS, WORD, WORD, WORD, DWORD, PCWSTR*, PCWSTR*);
using FreeInstanceFn = VOID (WINAPI*)(PDNS_SERVICE_INSTANCE);
using RegisterFn = DWORD (WINAPI*)(PDNS_SERVICE_REGISTER_REQUEST, PDNS_SERVICE_CANCEL);

// Resolved at run time so the application still starts on older Windows,
// where only QR pairing is available.
struct DnsSdApi {
    ConstructInstanceFn construct{};
    FreeInstanceFn freeInstance{};
    RegisterFn registerService{};
    RegisterFn deregisterService{};

    static const DnsSdApi* get() {
        static const DnsSdApi api = [] {
            DnsSdApi loaded;
            HMODULE module = LoadLibraryW(L"dnsapi.dll");
            if (!module) return loaded;
            loaded.construct = reinterpret_cast<ConstructInstanceFn>(
                GetProcAddress(module, "DnsServiceConstructInstance"));
            loaded.freeInstance = reinterpret_cast<FreeInstanceFn>(
                GetProcAddress(module, "DnsServiceFreeInstance"));
            loaded.registerService = reinterpret_cast<RegisterFn>(
                GetProcAddress(module, "DnsServiceRegister"));
            loaded.deregisterService = reinterpret_cast<RegisterFn>(
                GetProcAddress(module, "DnsServiceDeRegister"));
            return loaded;
        }();
        return api.construct && api.freeInstance && api.registerService &&
            api.deregisterService ? &api : nullptr;
    }
};

std::wstring wide(const std::string& text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        out.data(), size);
    return out;
}

} // namespace
#endif

// ── Other POSIX (Linux): a minimal mDNS responder ─────────────────────────
#if !defined(_WIN32) && !defined(__APPLE__)
namespace {

constexpr uint16_t kMdnsPort = 5353;
constexpr const char* kMdnsGroup = "224.0.0.251";
constexpr uint16_t kTypeA = 1;
constexpr uint16_t kTypePtr = 12;
constexpr uint16_t kTypeTxt = 16;
constexpr uint16_t kTypeSrv = 33;
constexpr uint16_t kTypeAny = 255;
constexpr uint16_t kClassIn = 1;
constexpr uint16_t kCacheFlush = 0x8000;

using Name = std::vector<std::string>;

struct Record {
    Name name;
    uint16_t type{};
    bool flush{};
    uint32_t ttl{};
    std::vector<uint8_t> data;
};

void putU16(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value));
}

void putU32(std::vector<uint8_t>& out, uint32_t value) {
    putU16(out, static_cast<uint16_t>(value >> 16));
    putU16(out, static_cast<uint16_t>(value));
}

void putName(std::vector<uint8_t>& out, const Name& name) {
    for (const auto& label : name) {
        out.push_back(static_cast<uint8_t>(label.size()));
        out.insert(out.end(), label.begin(), label.end());
    }
    out.push_back(0);
}

bool sameName(const Name& left, const Name& right) {
    if (left.size() != right.size()) return false;
    for (size_t index = 0; index < left.size(); ++index) {
        if (left[index].size() != right[index].size()) return false;
        for (size_t at = 0; at < left[index].size(); ++at)
            if (std::tolower(static_cast<unsigned char>(left[index][at])) !=
                std::tolower(static_cast<unsigned char>(right[index][at]))) return false;
    }
    return true;
}

// Reads a possibly compressed name; advances offset past it.
bool readName(const uint8_t* packet, size_t size, size_t& offset, Name& name) {
    size_t at = offset;
    bool jumped = false;
    for (int guard = 0; guard < 128; ++guard) {
        if (at >= size) return false;
        const uint8_t length = packet[at];
        if (length == 0) {
            if (!jumped) offset = at + 1;
            return true;
        }
        if ((length & 0xc0) == 0xc0) {
            if (at + 1 >= size) return false;
            if (!jumped) offset = at + 2;
            at = static_cast<size_t>(((length & 0x3f) << 8) | packet[at + 1]);
            jumped = true;
            continue;
        }
        if (length > 63 || at + 1 + length > size) return false;
        name.emplace_back(reinterpret_cast<const char*>(packet + at + 1), length);
        at += 1 + length;
    }
    return false;
}

} // namespace
#endif

struct PairDiscoveryAdvertiser::Impl {
    std::mutex mutex;
    std::condition_variable wake;
    PairServiceInfo info;
    bool dirty{};
    bool stopping{};
    std::thread thread;

#ifdef _WIN32
    std::unique_ptr<WinsockGuard> winsock;
    const DnsSdApi* api{};
    PDNS_SERVICE_INSTANCE instance{};
    DNS_SERVICE_REGISTER_REQUEST request{};
    HANDLE completed{};
    std::wstring serviceName;
    std::wstring hostName;
    std::wstring serverId;
    IP4_ADDRESS ip4{};

    static VOID WINAPI onComplete(DWORD, PVOID context, PDNS_SERVICE_INSTANCE result) {
        auto* self = static_cast<Impl*>(context);
        if (result && self->api) self->api->freeInstance(result);
        SetEvent(self->completed);
    }

    void withdraw() {
        if (!instance) return;
        ResetEvent(completed);
        if (api->deregisterService(&request, nullptr) == DNS_REQUEST_PENDING)
            WaitForSingleObject(completed, 2000);
        api->freeInstance(instance);
        instance = nullptr;
    }

    void publish(const PairServiceInfo& current, const std::string& address) {
        withdraw();
        if (address.empty()) return;
        in_addr parsed{};
        if (inet_pton(AF_INET, address.c_str(), &parsed) != 1) return;
        ip4 = parsed.s_addr;
        serviceName = wide(instanceLabel(current.name) + "." + kServiceType + ".local");
        hostName = wide(hostLabel(current.serverId) + ".local");
        serverId = wide(current.serverId);
        PCWSTR keys[] = {L"v", L"id", L"pair"};
        PCWSTR values[] = {L"3", serverId.c_str(), current.pairing ? L"1" : L"0"};
        instance = api->construct(serviceName.c_str(), hostName.c_str(), &ip4, nullptr,
                                  current.port, 0, 0, 3, keys, values);
        if (!instance) return;
        request = {};
        request.Version = DNS_QUERY_REQUEST_VERSION1;
        request.InterfaceIndex = 0;
        request.pServiceInstance = instance;
        request.pRegisterCompletionCallback = &Impl::onComplete;
        request.pQueryContext = this;
        request.unicastEnabled = FALSE;
        ResetEvent(completed);
        if (api->registerService(&request, nullptr) != DNS_REQUEST_PENDING) {
            api->freeInstance(instance);
            instance = nullptr;
        }
    }

    bool open(std::string* error) {
        api = DnsSdApi::get();
        if (!api) {
            if (error) *error = "LAN discovery needs Windows 10 version 1809 or later";
            return false;
        }
        winsock = std::make_unique<WinsockGuard>();
        completed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        return completed != nullptr;
    }

    void close() {
        withdraw();
        if (completed) CloseHandle(completed);
        completed = nullptr;
        winsock.reset();
    }

    void run() {
        std::string published;
        std::unique_lock lock(mutex);
        while (!stopping) {
            const PairServiceInfo current = info;
            const bool changed = dirty;
            dirty = false;
            lock.unlock();
            const std::string address = primaryIpv4Address();
            if (changed || address != published) {
                publish(current, address);
                published = address;
            }
            lock.lock();
            wake.wait_for(lock, kAddressCheck, [this] { return stopping || dirty; });
        }
    }
#elif defined(__APPLE__)
    DNSServiceRef ref{};

    void withdraw() {
        if (ref) DNSServiceRefDeallocate(ref);
        ref = nullptr;
    }

    void publish(const PairServiceInfo& current) {
        withdraw();
        TXTRecordRef txt;
        TXTRecordCreate(&txt, 0, nullptr);
        TXTRecordSetValue(&txt, "v", 1, kTxtVersion);
        TXTRecordSetValue(&txt, "id", static_cast<uint8_t>(current.serverId.size()),
                          current.serverId.data());
        TXTRecordSetValue(&txt, "pair", 1, current.pairing ? "1" : "0");
        const std::string label = instanceLabel(current.name);
        // The system responder chooses the host name and follows address
        // changes itself.
        if (DNSServiceRegister(&ref, 0, 0, label.c_str(), kServiceType, nullptr, nullptr,
                               htons(current.port), TXTRecordGetLength(&txt),
                               TXTRecordGetBytesPtr(&txt), nullptr, nullptr) !=
            kDNSServiceErr_NoError) ref = nullptr;
        TXTRecordDeallocate(&txt);
    }

    bool open(std::string*) { return true; }
    void close() { withdraw(); }

    void run() {
        std::unique_lock lock(mutex);
        bool first = true;
        while (!stopping) {
            if (first || dirty) {
                const PairServiceInfo current = info;
                dirty = false;
                first = false;
                lock.unlock();
                publish(current);
                lock.lock();
            }
            wake.wait(lock, [this] { return stopping || dirty; });
        }
    }
#else
    Socket socket{kInvalidSocket};

    bool open(std::string* error) {
        socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket == kInvalidSocket) {
            if (error) *error = "Unable to create the discovery socket";
            return false;
        }
        // The system's mDNS daemon usually holds 5353 too; multicast is
        // delivered to every socket that shares the port.
        const int enabled = 1;
        setsockopt(socket, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
#ifdef SO_REUSEPORT
        setsockopt(socket, SOL_SOCKET, SO_REUSEPORT, &enabled, sizeof(enabled));
#endif
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_port = htons(kMdnsPort);
        local.sin_addr.s_addr = htonl(INADDR_ANY);
        ip_mreq membership{};
        inet_pton(AF_INET, kMdnsGroup, &membership.imr_multiaddr);
        membership.imr_interface.s_addr = htonl(INADDR_ANY);
        if (bind(socket, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0 ||
            setsockopt(socket, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                       &membership, sizeof(membership)) != 0) {
            if (error) *error = "Unable to join the mDNS group";
            closeSocket(socket);
            socket = kInvalidSocket;
            return false;
        }
        const unsigned char ttl = 255;
        setsockopt(socket, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
        timeval timeout{0, 500000};
        setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        return true;
    }

    void close() {
        if (socket != kInvalidSocket) closeSocket(socket);
        socket = kInvalidSocket;
    }

    struct Names {
        Name service;
        Name instance;
        Name host;
        Name enumeration{"_services", "_dns-sd", "_udp", "local"};
    };

    static Names namesFor(const PairServiceInfo& current) {
        Names names;
        names.service = {"_tracknrace-pair", "_tcp", "local"};
        names.instance = {instanceLabel(current.name), "_tracknrace-pair", "_tcp", "local"};
        names.host = {hostLabel(current.serverId), "local"};
        return names;
    }

    static Record ptr(const Names& names, uint32_t ttl) {
        Record record{names.service, kTypePtr, false, ttl, {}};
        putName(record.data, names.instance);
        return record;
    }

    static Record srv(const Names& names, const PairServiceInfo& current, uint32_t ttl) {
        Record record{names.instance, kTypeSrv, true, ttl, {}};
        putU16(record.data, 0);
        putU16(record.data, 0);
        putU16(record.data, current.port);
        putName(record.data, names.host);
        return record;
    }

    static Record txt(const Names& names, const PairServiceInfo& current, uint32_t ttl) {
        Record record{names.instance, kTypeTxt, true, ttl, {}};
        const std::string entries[] = {
            std::string("v=") + kTxtVersion,
            "id=" + current.serverId.substr(0, 200),
            std::string("pair=") + (current.pairing ? "1" : "0"),
        };
        for (const auto& entry : entries) {
            record.data.push_back(static_cast<uint8_t>(entry.size()));
            record.data.insert(record.data.end(), entry.begin(), entry.end());
        }
        return record;
    }

    static std::optional<Record> address(const Names& names, const std::string& ip, uint32_t ttl) {
        in_addr parsed{};
        if (ip.empty() || inet_pton(AF_INET, ip.c_str(), &parsed) != 1) return std::nullopt;
        Record record{names.host, kTypeA, true, ttl, {}};
        const auto* bytes = reinterpret_cast<const uint8_t*>(&parsed.s_addr);
        record.data.assign(bytes, bytes + 4);
        return record;
    }

    void send(const std::vector<Record>& answers, const std::vector<Record>& additional,
              const sockaddr_in* unicastTo) {
        if (answers.empty()) return;
        std::vector<uint8_t> packet;
        putU16(packet, 0);       // id
        putU16(packet, 0x8400);  // response, authoritative
        putU16(packet, 0);
        putU16(packet, static_cast<uint16_t>(answers.size()));
        putU16(packet, 0);
        putU16(packet, static_cast<uint16_t>(additional.size()));
        for (const auto* section : {&answers, &additional}) {
            for (const auto& record : *section) {
                putName(packet, record.name);
                putU16(packet, record.type);
                putU16(packet, static_cast<uint16_t>(kClassIn | (record.flush ? kCacheFlush : 0)));
                putU32(packet, record.ttl);
                putU16(packet, static_cast<uint16_t>(record.data.size()));
                packet.insert(packet.end(), record.data.begin(), record.data.end());
            }
        }
        sockaddr_in destination{};
        if (unicastTo) {
            destination = *unicastTo;
        } else {
            destination.sin_family = AF_INET;
            destination.sin_port = htons(kMdnsPort);
            inet_pton(AF_INET, kMdnsGroup, &destination.sin_addr);
        }
        sendto(socket, packet.data(), packet.size(), 0,
               reinterpret_cast<sockaddr*>(&destination), sizeof(destination));
    }

    void announce(const PairServiceInfo& current, const std::string& ip, bool goodbye) {
        const Names names = namesFor(current);
        const uint32_t shortTtl = goodbye ? 0 : 120;
        const uint32_t longTtl = goodbye ? 0 : 4500;
        std::vector<Record> answers{ptr(names, longTtl), srv(names, current, shortTtl),
                                    txt(names, current, longTtl)};
        if (auto a = address(names, ip, shortTtl)) answers.push_back(std::move(*a));
        send(answers, {}, nullptr);
    }

    void answer(const uint8_t* packet, size_t size, const sockaddr_in& sender,
                const PairServiceInfo& current, const std::string& ip) {
        if (size < 12) return;
        const uint16_t flags = static_cast<uint16_t>(packet[2] << 8 | packet[3]);
        if (flags & 0x8000) return;  // a response, not a query
        const uint16_t questions = static_cast<uint16_t>(packet[4] << 8 | packet[5]);
        const Names names = namesFor(current);
        std::vector<Record> answers;
        std::vector<Record> additional;
        bool unicast = ntohs(sender.sin_port) != kMdnsPort;
        size_t offset = 12;
        for (uint16_t index = 0; index < questions && index < 32; ++index) {
            Name name;
            if (!readName(packet, size, offset, name) || offset + 4 > size) return;
            const uint16_t type = static_cast<uint16_t>(packet[offset] << 8 | packet[offset + 1]);
            const uint16_t klass = static_cast<uint16_t>(packet[offset + 2] << 8 | packet[offset + 3]);
            offset += 4;
            if (klass & 0x8000) unicast = true;
            const bool any = type == kTypeAny;
            if (sameName(name, names.enumeration) && (type == kTypePtr || any)) {
                Record record{names.enumeration, kTypePtr, false, 4500, {}};
                putName(record.data, names.service);
                answers.push_back(std::move(record));
            } else if (sameName(name, names.service) && (type == kTypePtr || any)) {
                answers.push_back(ptr(names, 4500));
                additional.push_back(srv(names, current, 120));
                additional.push_back(txt(names, current, 4500));
                if (auto a = address(names, ip, 120)) additional.push_back(std::move(*a));
            } else if (sameName(name, names.instance) &&
                       (type == kTypeSrv || type == kTypeTxt || any)) {
                if (type != kTypeTxt) answers.push_back(srv(names, current, 120));
                if (type != kTypeSrv) answers.push_back(txt(names, current, 4500));
                if (auto a = address(names, ip, 120)) additional.push_back(std::move(*a));
            } else if (sameName(name, names.host) && (type == kTypeA || any)) {
                if (auto a = address(names, ip, 120)) answers.push_back(std::move(*a));
            }
        }
        send(answers, additional, unicast ? &sender : nullptr);
    }

    void run() {
        std::string published;
        PairServiceInfo announced;
        auto nextAddressCheck = std::chrono::steady_clock::now();
        std::array<uint8_t, 1500> buffer{};
        while (true) {
            PairServiceInfo current;
            bool changed = false;
            {
                std::lock_guard lock(mutex);
                if (stopping) break;
                current = info;
                changed = dirty;
                dirty = false;
            }
            const auto now = std::chrono::steady_clock::now();
            if (changed || now >= nextAddressCheck) {
                const std::string ip = primaryIpv4Address();
                if (changed || ip != published) {
                    announce(current, ip, false);
                    published = ip;
                    announced = current;
                }
                nextAddressCheck = now + kAddressCheck;
            }
            sockaddr_in sender{};
            socklen_t senderSize = sizeof(sender);
            const ssize_t count = recvfrom(socket, buffer.data(), buffer.size(), 0,
                reinterpret_cast<sockaddr*>(&sender), &senderSize);
            if (count > 0) answer(buffer.data(), static_cast<size_t>(count), sender,
                                  current, published);
        }
        if (!published.empty()) announce(announced, published, true);
    }
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
    if (!impl_->open(error)) {
        impl_->close();
        return false;
    }
    {
        std::lock_guard lock(impl_->mutex);
        impl_->info = std::move(info);
        impl_->dirty = true;
        impl_->stopping = false;
    }
    running_.store(true);
    impl_->thread = std::thread([this] { impl_->run(); });
    return true;
}

void PairDiscoveryAdvertiser::update(PairServiceInfo info) {
    if (!validToken(info.serverId, 128) || !validToken(info.name, 96) || info.port == 0) return;
    {
        std::lock_guard lock(impl_->mutex);
        impl_->info = std::move(info);
        impl_->dirty = true;
    }
    impl_->wake.notify_all();
}

void PairDiscoveryAdvertiser::stop() {
    {
        std::lock_guard lock(impl_->mutex);
        impl_->stopping = true;
    }
    impl_->wake.notify_all();
    if (impl_->thread.joinable()) impl_->thread.join();
    if (running_.exchange(false)) impl_->close();
}

} // namespace tnrp
