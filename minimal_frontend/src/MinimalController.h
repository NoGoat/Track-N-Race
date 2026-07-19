#pragma once

#include "AppSettings.h"

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>

#include <tnrp/Engine.h>
#include <tnrp/Sink.h>

class MinimalController final : private tnrp::Sink {
public:
    using SessionCallback = std::function<void(std::string circuit, std::string session)>;
    using RecordingCallback = std::function<void(std::string status, std::string error)>;

    explicit MinimalController(AppSettings settings);
    ~MinimalController() override;

    MinimalController(const MinimalController&) = delete;
    MinimalController& operator=(const MinimalController&) = delete;

    void setSessionCallback(SessionCallback callback);
    void setRecordingCallback(RecordingCallback callback);
    bool start(std::string& error);

    bool setOutputFolder(const std::string& folder, std::string& error);
    void setProtocol(tnrp::Override protocol);
    bool applyNetwork(const std::string& bindAddress, uint16_t port, std::string& error);

    const AppSettings& settings() const noexcept { return settings_; }

    static bool validateOutputFolder(const std::string& folder, std::string& error);
    static bool validateIpv4(const std::string& address);

private:
    void onRow(const std::string& json) override;
    void beginRecordingWatch();
    void checkRecordingFile();
    void publishRecordingState(std::string status, std::string error = {});
    void publishError(const std::string& error);

    AppSettings settings_;
    std::unique_ptr<tnrp::Engine> engine_;
    std::mutex callbackMutex_;
    SessionCallback sessionCallback_;
    RecordingCallback recordingCallback_;
    std::string recordingStatus_{"Not recording"};
    std::string recordingError_;
    std::unordered_set<std::string> knownRecordings_;
    std::chrono::steady_clock::time_point firstSessionSeen_{};
    bool sessionSeen_{};
    bool recordingFileSeen_{};
};
