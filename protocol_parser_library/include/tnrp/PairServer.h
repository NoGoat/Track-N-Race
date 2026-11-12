#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tnrp {

// A playback_lap_blocks row as a paired phone receives it: each block's lap,
// times and fitted compound, without the per-block status history, plus the
// recording's race events for the Session tab. Also used by the Android app
// for recordings it plays itself.
std::string pairPhoneLapBlocks(const std::string& json);

// One driver's laps from a full playback_lap_blocks row's analysisDrivers, in
// the same block and lap shape as pairPhoneLapBlocks: {"blocks","laps",
// "fastestLapNum"}. Empty when the row holds no such driver (only V6
// recordings list every driver).
std::string pairPhoneDriverLaps(const std::string& lapBlocksJson, int driverIndex);

struct PairServerConfig {
    uint16_t port = 20779;
    std::string name{"Track N Race"};
    std::string persistedStateJson;
    bool enabled = false;
};

// Shared paired-display transport owned by libtnrp. Hosts only provide an
// identity label, persist the opaque private-state JSON (it holds the signing
// seed, so hosts store it encrypted), and display the public state JSON. The
// server owns discovery, the encrypted channel (PairCrypto.h), authentication,
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
    // Each lap's driver is a car index, or -1 for the selected driver.
    using LapDeltaCallback = std::function<std::string(int currentLap,
                                                       int comparisonLap,
                                                       bool sectorDelta,
                                                       int currentDriver,
                                                       int comparisonDriver)>;
    // One recorded lap of a driver (-1 = the selected driver), reduced to the
    // "family.field" channels a phone's Analysis page draws. Empty when
    // nothing is playing.
    using LapDataCallback = std::function<std::string(int lapNum,
                                                      const std::vector<std::string>& channels,
                                                      int driverIndex)>;
    // One driver's lap catalogue for a phone's lap picker (pairPhoneDriverLaps).
    // Empty when nothing is playing or the recording holds no such driver.
    using DriverLapsCallback = std::function<std::string(int driverIndex)>;
    // The braking zones of one lap, or two matched up (comparisonLap 0 for
    // one), for a phone's Analysis map. Empty when nothing is playing.
    using BrakingZonesCallback = std::function<std::string(int currentLap,
                                                           int comparisonLap,
                                                           int currentDriver,
                                                           int comparisonDriver)>;
    using DiagnosticCallback = std::function<void(const std::string& message)>;
    // One car's driver_lap_history row (Engine::driverLapHistoryJson), for a
    // phone's Timing laps sheet. Empty when there is no such car.
    using DriverLapHistoryCallback = std::function<std::string(int carIdx)>;
    // One Analysis map layer painted for one lap, or two (comparisonLap 0 for
    // one), in the phone's theme and lap colours (Engine::playerAnalysisMapJson).
    // Empty when nothing is playing.
    using AnalysisMapCallback = std::function<std::string(const std::string& layer,
                                                          int currentLap, int comparisonLap,
                                                          int currentDriver, int comparisonDriver,
                                                          bool dark,
                                                          const std::string& currentColor,
                                                          const std::string& comparisonColor)>;

    PairServer();
    ~PairServer();
    PairServer(const PairServer&) = delete;
    PairServer& operator=(const PairServer&) = delete;

    void configure(PairServerConfig config, StateCallback stateCallback,
                   RequirementsCallback requirementsCallback,
                   LapDeltaCallback lapDeltaCallback,
                   LapDataCallback lapDataCallback,
                   DriverLapsCallback driverLapsCallback,
                   BrakingZonesCallback brakingZonesCallback,
                   DiagnosticCallback diagnosticCallback = {});
    // Answers request_driver_lap_history (advertised as "driver-lap-history");
    // the Engine sets it before configure(). Unset, requests go unanswered.
    void setDriverLapHistoryCallback(DriverLapHistoryCallback callback);
    // Answers request_analysis_map (advertised as "analysis-map"); the Engine
    // sets it before configure(). Unset, requests are answered with null data.
    void setAnalysisMapCallback(AnalysisMapCallback callback);

    bool start(std::string* error = nullptr);
    void stop(bool persistDisabled = true);
    void openPairingWindow();
    void closePairingWindow();
    // Allows or refuses the phone named in public state's pendingDevice. A
    // phone that proved the QR secret or code still needs this before it is
    // saved or receives telemetry.
    void respondToPairing(bool approve);
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
