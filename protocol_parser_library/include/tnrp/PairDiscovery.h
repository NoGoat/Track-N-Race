#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace tnrp {

// DNS-SD advertisement of the paired-display service, type
// _tracknrace-pair._tcp. TXT carries only the protocol version, the opaque
// server id and whether pairing is open: discovery is a hint, never proof of
// identity, and holds no secrets. Windows 10 1809+ and Apple use the system
// mDNS responder; other platforms answer queries from libtnrp.
struct PairServiceInfo {
    std::string serverId;
    std::string name;
    uint16_t port = 0;
    bool pairing = false;
};

class PairDiscoveryAdvertiser {
public:
    PairDiscoveryAdvertiser();
    ~PairDiscoveryAdvertiser();
    PairDiscoveryAdvertiser(const PairDiscoveryAdvertiser&) = delete;
    PairDiscoveryAdvertiser& operator=(const PairDiscoveryAdvertiser&) = delete;

    bool start(PairServiceInfo info, std::string* error = nullptr);
    void update(PairServiceInfo info);
    void stop();
    bool running() const { return running_.load(); }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::atomic<bool> running_{false};
};

// The IPv4 address of the interface that carries the default route, which is
// the one a phone on the same Wi-Fi can reach. Falls back to the first
// non-loopback address; empty if there is none.
std::string primaryIpv4Address();

} // namespace tnrp
