#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace tnrp {

// Manual protocol selection. Auto detects the F1 game year from the packet
// format word (bytes 0-1) with debounce; F1_24 / F1_25 / F1_26 force a parser.
enum class Override { Auto, F1_24, F1_25, F1_26 };

inline const char* toString(Override o) {
    switch (o) {
        case Override::F1_24: return "f1_24";
        case Override::F1_25: return "f1_25";
        case Override::F1_26: return "f1_26";
        default:              return "auto";
    }
}

inline Override overrideFromString(const std::string& s) {
    if (s == "f1_24") return Override::F1_24;
    if (s == "f1_25") return Override::F1_25;
    if (s == "f1_26") return Override::F1_26;
    return Override::Auto;
}

struct UdpForwardTarget {
    std::string address;
    uint16_t    port = 20777;
};

inline constexpr size_t kMaxUdpForwardTargets = 15;

// Runtime configuration for the engine. Supplied at construction (the bridge
// fills it from CLI args) and mutated live via Engine::setOverride / setLogging /
// setStrategyMinimumStops / restartUdp.
struct Config {
    uint16_t    port           = 20777;
    std::string bindAddress    = "0.0.0.0";
    Override    protocol       = Override::Auto;
    bool        loggingEnabled = false;
    std::string outputDirectory;        // where .tnrd files are written
    int         strategyMinimumStops = 1;

    // Raw datagrams are copied to these IPv4 destinations before parsing. The
    // listener caps this list at 15 even if a host supplies more.
    std::vector<UdpForwardTarget> udpForwardTargets;

    // Electron/N-API playback fast path. When true, playback emits the hot
    // 60 Hz rows (telemetry/motion/motion_ex) packed via Sink::onBinary and
    // seeks via Sink::onSeekFlush, the lap-blocks payload carries the slim
    // per-lap chart points, and the playback loop re-emits the sparse panel
    // rows each tick. Off by default: JSON-only consumers (the Qt recorder,
    // the stdio pipe) keep the legacy all-JSON playback stream.
    bool        binaryPlayback = false;

    // When true, the live UDP path emits the hot 60 Hz rows as JSON via
    // Sink::onRow() and skips the packed binary Sink::onBinary() channel. Off by
    // default: the Electron/node addon clients want the binary fast-path across
    // the process/N-API boundary. An in-process consumer (the native Qt recorder)
    // sets this so it receives one uniform JSON stream and need not decode binary.
    bool        hotRowsAsJson  = false;
};

} // namespace tnrp
