#pragma once

#include <cstdint>
#include <string>

namespace tnrp {

// Which drivers a recording keeps. AllDrivers writes every car (…-all.tnrd),
// DriverOnly just the player's per-car data (…-driver.tnrd). Both writes the
// two files side by side; Ask writes both and lets the host delete one or
// neither once the session finishes (RecordingFinishedRow::ask).
enum class RecordingScope : uint8_t { AllDrivers, DriverOnly, Both, Ask };

inline const char* toString(RecordingScope scope) {
    switch (scope) {
        case RecordingScope::DriverOnly: return "driver_only";
        case RecordingScope::Both:       return "both";
        case RecordingScope::Ask:        return "ask";
        default:                         return "all_drivers";
    }
}

inline RecordingScope recordingScopeFromString(const std::string& s) {
    if (s == "driver_only") return RecordingScope::DriverOnly;
    if (s == "both")        return RecordingScope::Both;
    if (s == "ask")         return RecordingScope::Ask;
    return RecordingScope::AllDrivers;
}

// The scope for each session category. Session types 1-4 are practice, 5-14
// qualifying (including sprint shootouts), 15-17 race and 18 time trial; an
// unknown session type keeps every driver.
struct RecordingScopes {
    RecordingScope practice   = RecordingScope::AllDrivers;
    RecordingScope qualifying = RecordingScope::AllDrivers;
    RecordingScope race       = RecordingScope::AllDrivers;
    RecordingScope timeTrial  = RecordingScope::AllDrivers;

    static const char* categoryFor(int sessionType) {
        if (sessionType >= 1 && sessionType <= 4)   return "practice";
        if (sessionType >= 5 && sessionType <= 14)  return "qualifying";
        if (sessionType >= 15 && sessionType <= 17) return "race";
        if (sessionType == 18)                      return "time_trial";
        return "unknown";
    }

    RecordingScope forSession(int sessionType) const {
        if (sessionType >= 1 && sessionType <= 4)   return practice;
        if (sessionType >= 5 && sessionType <= 14)  return qualifying;
        if (sessionType >= 15 && sessionType <= 17) return race;
        if (sessionType == 18)                      return timeTrial;
        return RecordingScope::AllDrivers;
    }
};

} // namespace tnrp
