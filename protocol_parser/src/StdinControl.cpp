#include "StdinControl.h"

#include <iostream>
#include <string>

void StdinControl::run() {
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        nlohmann::json cmd;
        try {
            cmd = nlohmann::json::parse(line);
        } catch (...) {
            std::cerr << "[bridge] bad command JSON: " << line << "\n";
            continue;
        }
        handle(cmd);
    }
    // stdin closed (parent quit) -> caller shuts the engine down.
}

void StdinControl::handle(const nlohmann::json& cmd) {
    const std::string c = cmd.value("cmd", std::string{});

    if (c == "set_override") {
        engine_.setOverride(tnrp::overrideFromString(cmd.value("value", "auto")));
    } else if (c == "set_logging") {
        engine_.setLogging(cmd.value("enabled", false), cmd.value("dir", std::string{}));
    } else if (c == "restart_udp") {
        engine_.restartUdp((uint16_t)cmd.value("port", 20777), cmd.value("bind", "0.0.0.0"));
    } else if (c == "player_load") {
        engine_.playerLoad(cmd.value("path", std::string{}));
    } else if (c == "player_play") {
        engine_.playerPlay();
    } else if (c == "player_pause") {
        engine_.playerPause();
    } else if (c == "player_close") {
        engine_.playerClose();
    } else if (c == "player_seek") {
        engine_.playerSeek(cmd.value("pct", 0.0f));
    } else if (c == "player_set_speed") {
        engine_.playerSetSpeed(cmd.value("mult", 1.0f));
    } else if (c == "player_get_lap_data") {
        engine_.playerGetLapData(cmd.value("lap_num", 0));
    } else {
        std::cerr << "[bridge] unknown cmd: " << c << "\n";
    }
}
