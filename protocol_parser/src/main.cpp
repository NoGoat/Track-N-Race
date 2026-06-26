#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

#include <tnrp/Config.h>
#include <tnrp/Engine.h>

#include "PipeSink.h"
#include "StdinControl.h"

// Headless telemetry bridge: owns UDP port 20777, parses via libtnrp, records
// .tnrd files, and streams parsed rows to Electron over a named pipe / domain
// socket. Spawned by Electron with initial config as CLI args; live control via
// stdin; dies when its stdin (parent) closes.

namespace {

std::string argValue(int argc, char** argv, const std::string& key, const std::string& def) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == key && i + 1 < argc) return argv[i + 1];
        std::string pfx = key + "=";
        if (a.rfind(pfx, 0) == 0) return a.substr(pfx.size());
    }
    return def;
}

bool argFlag(int argc, char** argv, const std::string& key) {
    const std::string pfx = key + "=";
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == key) return true;                    // bare flag present
        if (a.rfind(pfx, 0) == 0) {                   // --flag=value form
            std::string v = a.substr(pfx.size());
            return v == "1" || v == "true" || v == "yes";
        }
    }
    return false;
}

} // namespace

int main(int argc, char** argv) {
    tnrp::Config config;
    config.port           = (uint16_t)std::stoi(argValue(argc, argv, "--port", "20777"));
    config.bindAddress    = argValue(argc, argv, "--bind", "0.0.0.0");
    config.protocol       = tnrp::overrideFromString(argValue(argc, argv, "--protocol", "auto"));
    config.loggingEnabled = argFlag(argc, argv, "--log-enabled");
    config.outputDirectory= argValue(argc, argv, "--log-dir", "");

    const std::string pipePath = argValue(argc, argv, "--pipe", "");
    if (pipePath.empty()) {
        std::cerr << "[bridge] missing required --pipe <name/path>\n";
        return 2;
    }

    PipeSink sink;
    if (!sink.connectTo(pipePath)) {
        std::cerr << "[bridge] failed to connect to pipe: " << pipePath << "\n";
        return 3;
    }

    tnrp::Engine engine(config, &sink);
    if (!engine.startUdp())
        std::cerr << "[bridge] UDP bind failed on port " << config.port << "\n";
    else
        std::cerr << "[bridge] listening on UDP " << config.bindAddress << ":" << config.port << "\n";

    // Blocks until stdin EOF (Electron quit) — then the Engine destructor stops
    // the UDP/playback threads and closes any active .tnrd file.
    StdinControl(engine).run();

    std::cerr << "[bridge] stdin closed, shutting down\n";
    return 0;
}
