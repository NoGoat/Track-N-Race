#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace tnrp {

// Cross-platform LAN discovery for paired displays. The wire format is a
// versioned multicast beacon owned by libtnrp so every host sees identical
// validation and endpoint semantics.
struct PairServiceInfo {
    std::string serverId;
    std::string name;
    uint16_t port = 0;
    bool pairing = false;
};

struct DiscoveredPairService {
    std::string serverId;
    std::string name;
    std::string address;
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
    std::thread thread_;
};

class PairDiscoveryBrowser {
public:
    using Callback = std::function<void(const DiscoveredPairService&)>;

    PairDiscoveryBrowser();
    ~PairDiscoveryBrowser();
    PairDiscoveryBrowser(const PairDiscoveryBrowser&) = delete;
    PairDiscoveryBrowser& operator=(const PairDiscoveryBrowser&) = delete;

    bool start(Callback callback, std::string* error = nullptr);
    void stop();
    bool running() const { return running_.load(); }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::atomic<bool> running_{false};
    std::thread thread_;
};

} // namespace tnrp
