#pragma once

#include <string>

#include <tnrp/Engine.h>

// One newline-delimited stdin command. Fields are read by glaze with
// error_on_unknown_keys=false; absent keys keep these defaults (mirrors the old
// nlohmann cmd.value(key, default) behaviour).
struct BridgeCommand {
    std::string cmd;
    std::string value   = "auto";       // set_override
    bool        enabled = false;        // set_logging
    std::string dir;                    // set_logging
    int         port    = 20777;        // restart_udp
    std::string bind    = "0.0.0.0";    // restart_udp
    std::string path;                   // player_load
    float       pct     = 0.0f;         // player_seek
    float       mult    = 1.0f;         // player_set_speed
    int         lap_num = 0;            // player_get_lap_data
};

// Reads newline-delimited JSON command objects from stdin and applies them to the
// Engine. Runs on the calling thread and blocks until stdin reaches EOF (which
// happens when Electron closes the child's stdin on quit), then returns so main()
// can shut down.
//
// Commands:
//   {"cmd":"set_override","value":"auto|f1_24|f1_25"}
//   {"cmd":"set_logging","enabled":true,"dir":"/path"}
//   {"cmd":"restart_udp","port":20777,"bind":"0.0.0.0"}
//   {"cmd":"player_load","path":"/file.tnrd"}
//   {"cmd":"player_play"} | "player_pause" | "player_close"
//   {"cmd":"player_seek","pct":0.5}
//   {"cmd":"player_set_speed","mult":2.0}
class StdinControl {
public:
    explicit StdinControl(tnrp::Engine& engine) : engine_(engine) {}
    void run();   // blocks until stdin EOF

private:
    tnrp::Engine& engine_;
    void handle(const BridgeCommand& cmd);
};
