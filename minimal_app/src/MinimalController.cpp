#include "MinimalController.h"

#include <tnrp/AnyRow.h>
#include <tnrp/DisplayNames.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string_view>
#include <utility>
#include <variant>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <arpa/inet.h>
#endif

MinimalController::MinimalController(AppSettings settings)
    : settings_(std::move(settings)) {}

MinimalController::~MinimalController() = default;

void MinimalController::setSessionCallback(SessionCallback callback) {
    std::lock_guard<std::mutex> lock(callbackMutex_);
    sessionCallback_ = std::move(callback);
}

bool MinimalController::start(std::string& error) {
    if (engine_) return true;

    std::string folderError;
    const bool canRecord = validateOutputFolder(settings_.outputFolder, folderError);

    tnrp::Config config;
    config.port = settings_.port;
    config.bindAddress = settings_.bindAddress;
    config.protocol = settings_.protocol;
    config.loggingEnabled = canRecord;
    config.outputDirectory = canRecord ? settings_.outputFolder : std::string{};
    config.binaryPlayback = false;
    config.hotRowsAsJson = false;

    // Convert to the private Sink base here, while access is permitted. Passing
    // `this` directly makes std::make_unique attempt the conversion inside the
    // standard-library template, where the private base is inaccessible on MSVC.
    auto* sink = static_cast<tnrp::Sink*>(this);
    engine_ = std::make_unique<tnrp::Engine>(config, sink);
    if (!engine_->startUdp()) {
        error = engine_->udpLastError();
        engine_.reset();
        return false;
    }
    return true;
}

bool MinimalController::setOutputFolder(const std::string& folder, std::string& error) {
    if (!validateOutputFolder(folder, error)) return false;

    if (engine_) engine_->setLogging(false, settings_.outputFolder);
    settings_.outputFolder = folder;
    if (engine_) engine_->setLogging(true, settings_.outputFolder);
    return true;
}

void MinimalController::setProtocol(tnrp::Override protocol) {
    settings_.protocol = protocol;
    if (engine_) engine_->setOverride(protocol);
}

bool MinimalController::applyNetwork(const std::string& bindAddress, uint16_t port,
                                     std::string& error) {
    if (!validateIpv4(bindAddress)) {
        error = "Enter a valid IPv4 address, for example 0.0.0.0 or 127.0.0.1.";
        return false;
    }
    if (port == 0) {
        error = "The UDP port must be between 1 and 65535.";
        return false;
    }

    if (!engine_) {
        const std::string previousAddress = settings_.bindAddress;
        const uint16_t previousPort = settings_.port;
        settings_.bindAddress = bindAddress;
        settings_.port = port;
        if (start(error)) return true;
        settings_.bindAddress = previousAddress;
        settings_.port = previousPort;
        return false;
    }

    const std::string previousAddress = settings_.bindAddress;
    const uint16_t previousPort = settings_.port;
    if (!engine_->restartUdp(port, bindAddress)) {
        error = engine_->udpLastError();
        if (!engine_->restartUdp(previousPort, previousAddress)) {
            error += " The previous UDP endpoint could not be restored: "
                  + engine_->udpLastError();
        }
        return false;
    }

    settings_.bindAddress = bindAddress;
    settings_.port = port;
    return true;
}

bool MinimalController::validateOutputFolder(const std::string& folder, std::string& error) {
    if (folder.empty()) {
        error = "Select a recording folder before recording can start.";
        return false;
    }

    std::error_code ec;
    // Settings and picker paths are stored as UTF-8. Constructing from a
    // u8string keeps that encoding contract and replaces deprecated u8path().
    const std::u8string utf8Folder(folder.begin(), folder.end());
    const std::filesystem::path directory(utf8Folder);
    if (!std::filesystem::is_directory(directory, ec) || ec) {
        error = "The selected recording folder does not exist.";
        return false;
    }

    const auto nonce = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const std::filesystem::path probe =
        directory / (".tnr-minimal-write-test-" + std::to_string(nonce));
    {
        std::ofstream stream(probe, std::ios::binary | std::ios::trunc);
        if (!stream) {
            error = "The selected recording folder is not writable.";
            return false;
        }
    }
    std::filesystem::remove(probe, ec);
    if (ec) {
        error = "The selected recording folder is writable, but temporary files cannot be removed.";
        return false;
    }
    return true;
}

bool MinimalController::validateIpv4(const std::string& address) {
    in_addr parsed{};
    return !address.empty() && ::inet_pton(AF_INET, address.c_str(), &parsed) == 1;
}

void MinimalController::onRow(const std::string& json) {
    std::optional<tnrp::AnyRow> row = tnrp::parseRow(std::string_view(json));
    if (!row) return;

    const auto* session = std::get_if<tnrp::SessionRow>(&*row);
    if (!session) return;

    SessionCallback callback;
    {
        std::lock_guard<std::mutex> lock(callbackMutex_);
        callback = sessionCallback_;
    }
    if (callback) {
        callback(std::string(tnrp::circuitName(session->track_id)),
                 std::string(tnrp::sessionName(session->session_type)));
    }
}
