#include "protocol.h"
#include "f1_24.h"
#include "f1_25.h"
#include "f1_26.h"
#include <cctype>

std::string RecordingFilenamePrefix(uint16_t format) {
    switch (format) {
        case 2024: return F1_24::RecordingFilenamePrefix();
        case 2025: return F1_25::RecordingFilenamePrefix();
        case 2026: return F1_26::RecordingFilenamePrefix();
        default:   return "f1_unknown";
    }
}

std::string ReadString(const uint8_t* data, int offset, int length) {
    std::string str(reinterpret_cast<const char*>(data + offset), length);
    size_t nullPos = str.find('\0');
    if (nullPos != std::string::npos) {
        str = str.substr(0, nullPos);
    }
    while (!str.empty() && isspace((unsigned char)str.front())) str.erase(0, 1);
    while (!str.empty() && isspace((unsigned char)str.back())) str.pop_back();
    return str;
}

const std::unordered_map<int, std::string> TRACK_NAMES = {
    {0, "Australian Grand Prix"},
    {2, "Chinese Grand Prix"},
    {3, "Bahrain Grand Prix"},
    {4, "Spanish Grand Prix"},
    {5, "Monaco Grand Prix"},
    {6, "Canadian Grand Prix"},
    {7, "British Grand Prix"},
    {9, "Hungarian Grand Prix"},
    {10, "Belgian Grand Prix"},
    {11, "Italian Grand Prix"},
    {12, "Singapore Grand Prix"},
    {13, "Japanese Grand Prix"},
    {14, "Abu Dhabi Grand Prix"},
    {15, "United States Grand Prix"},
    {16, "São Paulo Grand Prix"},
    {17, "Austrian Grand Prix"},
    {19, "Mexico City Grand Prix"},
    {20, "Azerbaijan Grand Prix"},
    {26, "Dutch Grand Prix"},
    {27, "Emilia Romagna Grand Prix"},
    {29, "Saudi Arabian Grand Prix"},
    {30, "Miami Grand Prix"},
    {31, "Las Vegas Grand Prix"},
    {32, "Qatar Grand Prix"},
    {39, "British Grand Prix"},
    {40, "Austrian Grand Prix"},
    {41, "Dutch Grand Prix"},
    {42, "Madrid Grand Prix"}   // 2026 Season Pack
};

const std::unordered_map<int, std::string> SESSION_NAMES = {
    {0, "Unknown"}, {1, "Practice 1"}, {2, "Practice 2"}, {3, "Practice 3"},
    {4, "Short Practice"}, {5, "Qualifying 1"}, {6, "Qualifying 2"}, {7, "Qualifying 3"},
    {8, "Short Qualifying"}, {9, "One-Shot Qualifying"},
    {10, "Sprint Shootout 1"}, {11, "Sprint Shootout 2"}, {12, "Sprint Shootout 3"},
    {13, "Short Sprint Shootout"}, {14, "One-Shot Sprint Shootout"},
    {15, "Race"}, {16, "Race 2"}, {17, "Race 3"}, {18, "Time Trial"}
};
