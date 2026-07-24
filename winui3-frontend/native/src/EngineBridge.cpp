#include "EngineBridge.h"

#include <tnrp/Config.h>
#include <tnrp/Engine.h>
#include <tnrp/Sink.h>
#include <tnrp/XlsxExport.h>

#include <algorithm>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <string>

namespace {

thread_local std::string createError;

tnrp::Override protocolFromInt(int value) {
    switch (value) {
        case 1: return tnrp::Override::F1_24;
        case 2: return tnrp::Override::F1_25;
        case 3: return tnrp::Override::F1_26;
        default: return tnrp::Override::Auto;
    }
}

std::string stringOrEmpty(const char* value) {
    return value ? std::string(value) : std::string();
}

class Bridge final : public tnrp::Sink {
public:
    Bridge(std::uint16_t port, const char* bindAddress, int protocol,
           tnr_row_callback rowCallback, tnr_binary_callback binaryCallback,
           tnr_seek_callback seekCallback, void* context)
        : rowCallback_(rowCallback),
          binaryCallback_(binaryCallback),
          seekCallback_(seekCallback),
          context_(context) {
        tnrp::Config config;
        config.port = port;
        config.bindAddress = bindAddress && *bindAddress ? bindAddress : "0.0.0.0";
        config.protocol = protocolFromInt(protocol);
        config.binaryPlayback = true;
        config.hotRowsAsJson = false;
        engine_ = std::make_unique<tnrp::Engine>(config, this);
    }

    ~Bridge() override {
        // Stop/join native callback threads while the callback fields and
        // synchronization members are still alive.
        engine_.reset();
    }

    bool start() {
        if (engine_->startUdp()) return true;
        setError(engine_->udpLastError());
        return false;
    }

    bool restart(std::uint16_t port, const char* bindAddress) {
        if (engine_->restartUdp(
                port, bindAddress && *bindAddress ? bindAddress : "0.0.0.0")) {
            setError({});
            return true;
        }
        setError(engine_->udpLastError());
        return false;
    }

    bool playerLoad(const char* path) {
        std::string error;
        if (engine_->playerLoad(stringOrEmpty(path), &error)) {
            setError({});
            return true;
        }
        setError(std::move(error));
        return false;
    }

    tnrp::Engine& engine() { return *engine_; }

    void setError(std::string error) {
        std::lock_guard lock(errorMutex_);
        lastError_ = std::move(error);
    }

    std::string error() const {
        std::lock_guard lock(errorMutex_);
        return lastError_;
    }

private:
    void onRow(const std::string& json) override {
        if (rowCallback_) rowCallback_(json.data(), json.size(), context_);
    }

    void onBinary(const std::uint8_t* data, std::size_t length) override {
        if (binaryCallback_) binaryCallback_(data, length, context_);
    }

    void onSeekFlush(const std::uint8_t* data, std::size_t length,
                     const std::string& coldJson, float currentLapStart,
                     int lapNumber) override {
        if (seekCallback_) {
            seekCallback_(data, length, coldJson.data(), coldJson.size(),
                          currentLapStart, lapNumber, context_);
        }
    }

    tnr_row_callback rowCallback_{};
    tnr_binary_callback binaryCallback_{};
    tnr_seek_callback seekCallback_{};
    void* context_{};
    std::unique_ptr<tnrp::Engine> engine_;
    mutable std::mutex errorMutex_;
    std::string lastError_;
};

Bridge* bridge(void* handle) {
    return static_cast<Bridge*>(handle);
}

std::size_t copyString(const std::string& value, char* buffer, std::size_t size) {
    if (buffer && size > 0) {
        const std::size_t count = std::min(value.size(), size - 1);
        std::memcpy(buffer, value.data(), count);
        buffer[count] = '\0';
    }
    return value.size();
}

template <typename Action>
void runVoid(void* handle, Action action) {
    if (!handle) return;
    try {
        action(*bridge(handle));
    } catch (const std::exception& ex) {
        bridge(handle)->setError(ex.what());
    } catch (...) {
        bridge(handle)->setError("The native telemetry engine failed unexpectedly.");
    }
}

} // namespace

void* tnr_engine_create(std::uint16_t port, const char* bindAddress, int protocol,
                        tnr_row_callback rowCallback,
                        tnr_binary_callback binaryCallback,
                        tnr_seek_callback seekCallback, void* context) {
    try {
        createError.clear();
        return new Bridge(port, bindAddress, protocol, rowCallback, binaryCallback,
                          seekCallback, context);
    } catch (const std::exception& ex) {
        createError = ex.what();
    } catch (...) {
        createError = "The native telemetry engine could not be created.";
    }
    return nullptr;
}

void tnr_engine_destroy(void* handle) {
    delete bridge(handle);
}

int tnr_engine_start(void* handle) {
    if (!handle) return 0;
    try {
        return bridge(handle)->start() ? 1 : 0;
    } catch (const std::exception& ex) {
        bridge(handle)->setError(ex.what());
    } catch (...) {
        bridge(handle)->setError("The native telemetry engine could not start.");
    }
    return 0;
}

int tnr_engine_restart_udp(void* handle, std::uint16_t port,
                           const char* bindAddress) {
    if (!handle) return 0;
    try {
        return bridge(handle)->restart(port, bindAddress) ? 1 : 0;
    } catch (const std::exception& ex) {
        bridge(handle)->setError(ex.what());
    } catch (...) {
        bridge(handle)->setError("The native UDP listener could not restart.");
    }
    return 0;
}

void tnr_engine_set_protocol(void* handle, int protocol) {
    runVoid(handle, [protocol](Bridge& value) {
        value.engine().setOverride(protocolFromInt(protocol));
    });
}

void tnr_engine_set_logging(void* handle, int enabled,
                            const char* outputDirectory) {
    runVoid(handle, [enabled, outputDirectory](Bridge& value) {
        value.engine().setLogging(enabled != 0, stringOrEmpty(outputDirectory));
    });
}

int tnr_engine_player_load(void* handle, const char* path) {
    if (!handle) return 0;
    try {
        return bridge(handle)->playerLoad(path) ? 1 : 0;
    } catch (const std::exception& ex) {
        bridge(handle)->setError(ex.what());
    } catch (...) {
        bridge(handle)->setError("The recording could not be loaded.");
    }
    return 0;
}

void tnr_engine_player_play(void* handle) {
    runVoid(handle, [](Bridge& value) { value.engine().playerPlay(); });
}

void tnr_engine_player_pause(void* handle) {
    runVoid(handle, [](Bridge& value) { value.engine().playerPause(); });
}

void tnr_engine_player_seek(void* handle, float percentage) {
    runVoid(handle, [percentage](Bridge& value) {
        value.engine().playerSeek(percentage);
    });
}

void tnr_engine_player_set_speed(void* handle, float multiplier) {
    runVoid(handle, [multiplier](Bridge& value) {
        value.engine().playerSetSpeed(multiplier);
    });
}

void tnr_engine_player_get_lap_data(void* handle, int lapNumber) {
    runVoid(handle, [lapNumber](Bridge& value) {
        value.engine().playerGetLapData(lapNumber);
    });
}

void tnr_engine_player_close(void* handle) {
    runVoid(handle, [](Bridge& value) { value.engine().playerClose(); });
}

int tnr_engine_export_xlsx(void* handle, const char* sourcePath,
                           const char* destinationPath) {
    if (!handle) return 0;
    try {
        std::string error;
        const bool ok = tnrp::exportTnrdFileToXlsx(
            stringOrEmpty(sourcePath), stringOrEmpty(destinationPath), &error);
        bridge(handle)->setError(std::move(error));
        return ok ? 1 : 0;
    } catch (const std::exception& ex) {
        bridge(handle)->setError(ex.what());
    } catch (...) {
        bridge(handle)->setError("The recording could not be exported.");
    }
    return 0;
}

std::size_t tnr_engine_copy_last_error(void* handle, char* buffer,
                                       std::size_t bufferSize) {
    return copyString(handle ? bridge(handle)->error() : std::string(),
                      buffer, bufferSize);
}

std::size_t tnr_engine_copy_create_error(char* buffer, std::size_t bufferSize) {
    return copyString(createError, buffer, bufferSize);
}
