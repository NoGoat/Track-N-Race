#pragma once

#include "AppSettings.h"

#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include <tnrp/Engine.h>
#include <tnrp/Sink.h>

class MinimalController final : private tnrp::Sink {
public:
    using SessionCallback = std::function<void(std::string circuit, std::string session)>;

    explicit MinimalController(AppSettings settings);
    ~MinimalController() override;

    MinimalController(const MinimalController&) = delete;
    MinimalController& operator=(const MinimalController&) = delete;

    void setSessionCallback(SessionCallback callback);
    bool start(std::string& error);

    bool setOutputFolder(const std::string& folder, std::string& error);
    void setProtocol(tnrp::Override protocol);
    bool applyNetwork(const std::string& bindAddress, uint16_t port, std::string& error);

    const AppSettings& settings() const noexcept { return settings_; }

    static bool validateOutputFolder(const std::string& folder, std::string& error);
    static bool validateIpv4(const std::string& address);

private:
    void onRow(const std::string& json) override;

    AppSettings settings_;
    std::unique_ptr<tnrp::Engine> engine_;
    std::mutex callbackMutex_;
    SessionCallback sessionCallback_;
};
