#include "StdinControl.h"

#include <cstdint>
#include <iostream>
#include <string>

#include <glaze/glaze.hpp>

void StdinControl::run() {
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        BridgeCommand cmd;
        auto ec = glz::read<glz::opts{.error_on_unknown_keys = false}>(cmd, line);
        if (ec) {
            std::cerr << "[bridge] bad command JSON: " << line << "\n";
            continue;
        }
        handle(cmd);
    }
    // stdin closed (parent quit) -> caller shuts the engine down.
}

void StdinControl::handle(const BridgeCommand& cmd) {
    const std::string& c = cmd.cmd;

    if (c == "set_override") {
        engine_.setOverride(tnrp::overrideFromString(cmd.value));
    } else if (c == "set_logging") {
        engine_.setLogging(cmd.enabled, cmd.dir);
    } else if (c == "restart_udp") {
        engine_.restartUdp((uint16_t)cmd.port, cmd.bind);
    } else if (c == "player_load") {
        engine_.playerLoad(cmd.path);
    } else if (c == "player_play") {
        engine_.playerPlay();
    } else if (c == "player_pause") {
        engine_.playerPause();
    } else if (c == "player_close") {
        engine_.playerClose();
    } else if (c == "player_seek") {
        engine_.playerSeek(cmd.pct);
    } else if (c == "player_set_speed") {
        engine_.playerSetSpeed(cmd.mult);
    } else if (c == "player_get_lap_data") {
        engine_.playerGetLapData(cmd.lap_num);
    } else {
        std::cerr << "[bridge] unknown cmd: " << c << "\n";
    }
}
