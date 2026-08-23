#pragma once

#include <array>

struct SessionLayout {
    bool showGpName       = true;
    bool showMarshalZones = true;
    bool showTimeLeft     = true;
    bool showMap          = true;
    bool showProximity    = true;
    bool showEvents       = true;
    bool showWeather      = true;

    bool showHeader() const {
        return showGpName || showMarshalZones || showTimeLeft;
    }

    enum StatCard {
        TotalLaps,
        LapsRemaining,
        PitSpeedLimit,
        PitWindow,
        PitRejoin,
        TrackTemp,
        AirTemp,
        TrackLength,
        TimeOfDay,
        StatCardCount
    };

    std::array<bool, StatCardCount> cards = {
        true, true, true, true, true, true, true, true, true
    };

    static const char* cardKey(int idx) {
        static const char* keys[StatCardCount] = {
            "totalLaps", "lapsRemaining", "pitSpeedLimit", "pitWindow",
            "pitRejoin", "trackTemp", "airTemp", "trackLength", "timeOfDay"
        };
        return keys[idx];
    }

    static const char* cardLabel(int idx) {
        static const char* labels[StatCardCount] = {
            "Total Laps", "Laps Rem", "Pit Speed", "Pit Window",
            "Rejoin", "Track Temp", "Air Temp", "Track Len", "Time/Day"
        };
        return labels[idx];
    }
};
