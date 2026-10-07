#pragma once

// Electron's DamageLayout (appConfig.ts): the status bar cards, the car
// diagram and the wear bar tiles, each shown or hidden in Edit Layout.
struct DamageLayout {
    enum StatusCard { Engine, Gearbox, WingFault, ErsFault, EngineStatus, StatusCount };
    enum WearTile { Ice, Mguh, Mguk, Es, Ce, Tc, EngineWear, GearboxWear, WearCount };

    bool statusCards[StatusCount] = {true, true, true, true, true};
    bool showDiagram = true;
    bool wearTiles[WearCount] = {true, true, true, true, true, true, true, true};

    static const char* statusKey(int idx) {
        static const char* keys[StatusCount] = {
            "engine", "gearbox", "wingFault", "ersFault", "engineStatus"
        };
        return keys[idx];
    }
    // Layout editor labels.
    static const char* statusLabel(int idx) {
        static const char* labels[StatusCount] = {
            "Engine", "Gearbox", "DRS / Wing", "ERS", "Engine Status"
        };
        return labels[idx];
    }
    static const char* wearKey(int idx) {
        static const char* keys[WearCount] = {
            "ice", "mguh", "mguk", "es", "ce", "tc", "engine", "gearbox"
        };
        return keys[idx];
    }
    // ICE combustion engine, MGU-H heat / MGU-K kinetic motor-generators, ES
    // energy store, CE control electronics, TC turbocharger.
    static const char* wearName(int idx) {
        static const char* names[WearCount] = {
            "ICE", "MGU-H", "MGU-K", "ES", "CE", "TC", "Engine", "Gearbox"
        };
        return names[idx];
    }
};
