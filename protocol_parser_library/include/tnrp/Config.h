#pragma once

#include <cstdint>
#include <string>

namespace tnrp {

// Manual protocol selection. Auto detects the F1 game year from the packet
// format word (bytes 0-1) with debounce; F1_24 / F1_25 force a specific parser.
enum class Override { Auto, F1_24, F1_25 };

inline const char* toString(Override o) {
    switch (o) {
        case Override::F1_24: return "f1_24";
        case Override::F1_25: return "f1_25";
        default:              return "auto";
    }
}

inline Override overrideFromString(const std::string& s) {
    if (s == "f1_24") return Override::F1_24;
    if (s == "f1_25") return Override::F1_25;
    return Override::Auto;
}

// Runtime configuration for the engine. Supplied at construction (the bridge
// fills it from CLI args) and mutated live via Engine::setOverride / setLogging
// / restartUdp (driven by stdin commands from Electron).
struct Config {
    uint16_t    port           = 20777;
    std::string bindAddress    = "0.0.0.0";
    Override    protocol       = Override::Auto;
    bool        loggingEnabled = false;
    std::string outputDirectory;        // where .tnrd files are written
};

} // namespace tnrp
