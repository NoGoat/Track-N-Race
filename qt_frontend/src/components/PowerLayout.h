#pragma once

struct PowerLayout {
    static constexpr int CardCount = 7;
    bool cards[CardCount] = {true, true, true, true, true, true, true};

    bool showSplit = true;
    bool showHarvest = true;
    bool showStore = true;
    bool showFuel = true;

    static const char* cardKey(int idx) {
        static const char* keys[CardCount] = {
            "totalPower", "ice", "mguk", "split", "ersStore", "ersPct", "fuel"
        };
        return keys[idx];
    }

    static const char* cardLabel(int idx) {
        static const char* labels[CardCount] = {
            "Total Power", "ICE", "MGU-K", "Split", "ERS Store", "ERS %", "Fuel"
        };
        return labels[idx];
    }
};
