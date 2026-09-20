#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tnrp {

struct PairServerConfig {
    uint16_t port = 20779;
    std::string name{"Track N Race"};
    std::string persistedStateJson;
    bool enabled = false;
};

// Shared paired-display transport owned by libtnrp. Hosts only provide an
// identity label, persist the opaque private-state JSON, and display the public
// state JSON. The server owns discovery, WebSocket framing, authentication,
// subscriptions, latest-state caches, and per-client backpressure.
class PairServer {
public:
    using StateCallback = std::function<void(const std::string& publicStateJson,
                                             const std::string& persistedStateJson)>;
    // Union of every connected phone's subscription (plus the always-on roster
    // family): the row-family bitmask and the V6 field types (V6DataType ids)
    // they asked for. refreshSnapshot is true when a phone has just subscribed
    // and needs the current state of those rows re-emitted; the engine has to
    // do that itself during sparse V6 playback, where the server's latest-row
    // cache only ever holds the most recent single-field patch.
    using RequirementsCallback = std::function<void(uint32_t streamMask,
                                                    const std::vector<uint8_t>& v6Types,
                                                    bool refreshSnapshot)>;
    using LapDeltaCallback = std::function<std::string(int currentLap,
                                                       int comparisonLap,
                                                       bool sectorDelta)>;
    using DiagnosticCallback = std::function<void(const std::string& message)>;

    PairServer();
    ~PairServer();
    PairServer(const PairServer&) = delete;
    PairServer& operator=(const PairServer&) = delete;

    void configure(PairServerConfig config, StateCallback stateCallback,
                   RequirementsCallback requirementsCallback,
                   LapDeltaCallback lapDeltaCallback,
                   DiagnosticCallback diagnosticCallback = {});

    bool start(std::string* error = nullptr);
    void stop(bool persistDisabled = true);
    void openPairingWindow();
    void closePairingWindow();
    void removeDevice(const std::string& id);

    std::string publicStateJson() const;
    std::string persistedStateJson() const;

    // Engine output taps. The server keeps participants requested and cached
    // even with no connected phone; every other emitted family is cached for
    // immediate latest-state snapshots and filtered per peer.
    void noteSession(uint64_t sessionUid);
    void publishRow(const std::string& json);
    void publishBinary(const uint8_t* data, size_t length);
    void publishSeekSnapshot(const uint8_t* data, size_t length,
                             const std::string& coldJson);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tnrp
