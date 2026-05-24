#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <shobjidl.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <chrono>
#include <cmath>
#include <mutex>
#include <nlohmann/json.hpp>
#include <zlib.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(linker,"\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

// UI Elements
HWND hMainWindow = NULL;
HWND hBtnDirectory = NULL;
HWND hBtnToggle = NULL;
HWND hLblStatus = NULL;
HWND hLblDirectory = NULL;

std::wstring outputDirectory = L"";
std::atomic<bool> isRecording(false);
std::atomic<bool> shouldExit(false);
std::thread receiverThread;

// Native Dark / Light Theme Globals
HBRUSH hBgBrush = NULL;
COLORREF textColor = RGB(0, 0, 0);
COLORREF bgColor = RGB(255, 255, 255);

// Gzip session stream
gzFile activeGzip = NULL;
int currentTrackId = -1;
int currentSessionType = -1;
std::wstring activeGzipPath = L"";
std::mutex streamMutex;

// Telemetry parsing constants
const int HEADER_SIZE = 29;

// Packet IDs
const int PID_MOTION           = 0;
const int PID_SESSION          = 1;
const int PID_LAP_DATA         = 2;
const int PID_EVENT            = 3;
const int PID_PARTICIPANTS     = 4;
const int PID_CAR_TEL          = 6;
const int PID_CAR_STATUS       = 7;
const int PID_CAR_DAMAGE       = 10;
const int PID_SESSION_HISTORY  = 11;
const int PID_TYRE_SETS        = 12;
const int PID_MOTION_EX        = 13;

const std::unordered_set<int> FRAME_SAMPLED = { PID_MOTION, PID_CAR_TEL, PID_MOTION_EX };

const std::unordered_map<int, int> SLOW_RATE_MS = {
    { PID_SESSION, 0 },
    { PID_LAP_DATA, 500 },
    { PID_CAR_STATUS, 500 },
    { PID_CAR_DAMAGE, 500 },
    { PID_PARTICIPANTS, 5000 },
    { PID_EVENT, 0 }
};

// Mappings
const std::unordered_map<int, std::string> F1_24_TEAM_COLORS = {
    {0, "#27f4d2"}, {1, "#e80020"}, {2, "#3671c6"}, {3, "#64c4ff"},
    {4, "#229971"}, {5, "#0093cc"}, {6, "#6692ff"}, {7, "#e6002b"},
    {8, "#ff8000"}, {9, "#52e252"}, {41, "#8e8e8e"}, {104, "#8e8e8e"},
    {143, "#ecebeb"}, {144, "#ff4646"}, {145, "#005aff"}, {146, "#1b2c56"},
    {147, "#39ff14"}, {148, "#ff3c00"}, {149, "#ff7c00"}, {150, "#ff2828"},
    {151, "#0028ff"}, {152, "#ffb400"}, {153, "#ffff00"}
};

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
    {41, "Dutch Grand Prix"}
};

const std::unordered_map<int, std::string> SESSION_NAMES = {
    {0, "Unknown"}, {1, "Practice 1"}, {2, "Practice 2"}, {3, "Practice 3"},
    {4, "Short Practice"}, {5, "Qualifying 1"}, {6, "Qualifying 2"}, {7, "Qualifying 3"},
    {8, "Short Qualifying"}, {9, "One-Shot Qualifying"},
    {10, "Sprint Shootout 1"}, {11, "Sprint Shootout 2"}, {12, "Sprint Shootout 3"},
    {13, "Short Sprint Shootout"}, {14, "One-Shot Sprint Shootout"},
    {15, "Race"}, {16, "Race 2"}, {17, "Race 3"}, {18, "Time Trial"}
};

// Deduplication State
const std::unordered_set<std::string> DEDUPE_TYPES = {
    "session", "tyre_sets", "participants", "all_status", "status", "timing", "damage"
};
std::unordered_map<std::string, std::string> dedupeCache;

// Rate limiting state
std::unordered_map<int, uint32_t> lastFrameId;
std::unordered_map<int, uint64_t> lastSlowMs;

// Helper struct for telemetry header
struct PacketHeader {
    uint16_t packetFormat;
    uint8_t packetId;
    float sessionTime;
    uint32_t overallFrameId;
    uint8_t playerCarIndex;
};

// Helper read functions
inline float ReadFloat(const uint8_t* data, int offset) {
    return *(const float*)(data + offset);
}
inline uint32_t ReadUInt32(const uint8_t* data, int offset) {
    return *(const uint32_t*)(data + offset);
}
inline uint16_t ReadUInt16(const uint8_t* data, int offset) {
    return *(const uint16_t*)(data + offset);
}
inline int16_t ReadInt16(const uint8_t* data, int offset) {
    return *(const int16_t*)(data + offset);
}
inline uint8_t ReadUInt8(const uint8_t* data, int offset) {
    return data[offset];
}
inline int8_t ReadInt8(const uint8_t* data, int offset) {
    return (int8_t)data[offset];
}

// Math round utilities matching JS
inline double Round1(double v) {
    return std::round(v * 10.0) / 10.0;
}
inline double Round2(double v) {
    return std::round(v * 100.0) / 100.0;
}
inline double Round3(double v) {
    return std::round(v * 1000.0) / 1000.0;
}
inline double Round4(double v) {
    return std::round(v * 10000.0) / 10000.0;
}

std::string GetISOTimestamp() {
    SYSTEMTIME st;
    GetSystemTime(&st);
    char buf[64];
    sprintf_s(buf, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
              st.wYear, st.wMonth, st.wDay,
              st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return std::string(buf);
}

std::string GetFilenameTimestamp() {
    SYSTEMTIME st;
    GetSystemTime(&st);
    char buf[64];
    sprintf_s(buf, "%04d-%02d-%02dT%02d-%02d-%02d-%03dZ",
              st.wYear, st.wMonth, st.wDay,
              st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return std::string(buf);
}

std::string SanitizeName(std::string name) {
    std::string result = "";
    for (char c : name) {
        if (isalnum((unsigned char)c)) {
            result += tolower((unsigned char)c);
        } else {
            result += '_';
        }
    }
    return result;
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

// Windows native theme query
bool IsSystemDarkMode() {
    HKEY hKey;
    DWORD value = 1; // Default to Light Mode
    DWORD size = sizeof(value);
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        RegQueryValueExW(hKey, L"AppsUseLightTheme", NULL, NULL, (LPBYTE)&value, &size);
        RegCloseKey(hKey);
    }
    return value == 0;
}

void UpdateThemeColors() {
    bool isDark = IsSystemDarkMode();
    if (hBgBrush) {
        DeleteObject(hBgBrush);
    }
    if (isDark) {
        bgColor = RGB(32, 32, 32);
        textColor = RGB(255, 255, 255);
        hBgBrush = CreateSolidBrush(bgColor);
    } else {
        bgColor = GetSysColor(COLOR_WINDOW);
        textColor = GetSysColor(COLOR_WINDOWTEXT);
        hBgBrush = CreateSolidBrush(bgColor);
    }
    
    if (hMainWindow) {
        // Dynamic class background swap
        SetClassLongPtrW(hMainWindow, GCLP_HBRBACKGROUND, (LONG_PTR)hBgBrush);

        // Apply dark title bar (DWM)
        BOOL useDark = isDark ? TRUE : FALSE;
        DwmSetWindowAttribute(hMainWindow, DWMWA_USE_IMMERSIVE_DARK_MODE, &useDark, sizeof(useDark));
        
        // Native explorer light/dark control themes
        const wchar_t* themeName = isDark ? L"DarkMode_Explorer" : L"Explorer";
        if (hBtnDirectory) SetWindowTheme(hBtnDirectory, themeName, NULL);
        if (hBtnToggle) SetWindowTheme(hBtnToggle, themeName, NULL);

        // Redraw whole client area
        RedrawWindow(hMainWindow, NULL, NULL, RDW_ERASE | RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    }
}

// Dialog directory selection
void SelectDirectory() {
    IFileDialog *pfd = NULL;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pfd));
    if (SUCCEEDED(hr)) {
        DWORD dwOptions;
        if (SUCCEEDED(pfd->GetOptions(&dwOptions))) {
            pfd->SetOptions(dwOptions | FOS_PICKFOLDERS);
        }
        if (SUCCEEDED(pfd->Show(hMainWindow))) {
            IShellItem *psi;
            if (SUCCEEDED(pfd->GetResult(&psi))) {
                PWSTR pszFilePath;
                if (SUCCEEDED(psi->GetDisplayName(SIGDN_FILESYSPATH, &pszFilePath))) {
                    outputDirectory = pszFilePath;
                    SetWindowTextW(hLblDirectory, pszFilePath);
                    CoTaskMemFree(pszFilePath);
                }
                psi->Release();
            }
        }
        pfd->Release();
    }
}

// Gzip stream close
void CloseActiveStream() {
    if (activeGzip) {
        gzclose(activeGzip);
        activeGzip = NULL;
        SetWindowTextW(hLblStatus, L"Status: Idle");
    }
    currentTrackId = -1;
    currentSessionType = -1;
    activeGzipPath = L"";
    dedupeCache.clear();
}

// Gzip stream initialization
void StartNewStream(int trackId, int sessionType, int format) {
    CloseActiveStream();
    if (!isRecording || outputDirectory.empty()) return;

    std::string protocolStr = (format == 2024) ? "f1_24" : "f1_25";
    std::string trackName = "unknown";
    auto itTrack = TRACK_NAMES.find(trackId);
    if (itTrack != TRACK_NAMES.end()) {
        trackName = SanitizeName(itTrack->second);
    } else {
        trackName = "track_" + std::to_string(trackId);
    }

    std::string sessionName = "unknown";
    auto itSess = SESSION_NAMES.find(sessionType);
    if (itSess != SESSION_NAMES.end()) {
        sessionName = SanitizeName(itSess->second);
    } else {
        sessionName = "session_" + std::to_string(sessionType);
    }

    std::string filename = protocolStr + "_" + std::to_string(trackId) + "_" + trackName + "_" + sessionName + "_" + GetFilenameTimestamp() + ".tnrd";
    std::wstring filenameW(filename.begin(), filename.end());
    activeGzipPath = outputDirectory + L"\\" + filenameW;

    activeGzip = gzopen_w(activeGzipPath.c_str(), "wb");
    if (activeGzip) {
        nlohmann::json header;
        header["magic"] = "TNRD_V1";
        header["protocol"] = format;
        header["track_id"] = trackId;
        auto itTrackRaw = TRACK_NAMES.find(trackId);
        header["track_name"] = (itTrackRaw != TRACK_NAMES.end()) ? itTrackRaw->second : "Unknown";
        header["session_type"] = sessionType;
        auto itSessRaw = SESSION_NAMES.find(sessionType);
        header["session_name"] = (itSessRaw != SESSION_NAMES.end()) ? itSessRaw->second : "Unknown";

        auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        header["start_time"] = nowMs;

        std::string headerLine = header.dump() + "\n";
        gzwrite(activeGzip, headerLine.c_str(), (unsigned int)headerLine.size());

        currentTrackId = trackId;
        currentSessionType = sessionType;

        // UI Label update
        std::wstring statusStr = L"Recording: " + filenameW.substr(0, 30) + L"...";
        SetWindowTextW(hLblStatus, statusStr.c_str());
    }
}

// Deduplication filter
bool IsDuplicate(const std::string& type, const nlohmann::json& row) {
    if (DEDUPE_TYPES.find(type) == DEDUPE_TYPES.end()) return false;
    nlohmann::json clone = row;
    clone.erase("ts");
    clone.erase("session_time");
    std::string hash = clone.dump();
    auto it = dedupeCache.find(type);
    if (it != dedupeCache.end() && it->second == hash) return true;
    dedupeCache[type] = hash;
    return false;
}

// Writes a telemetry row to the gzip file
void RecordRow(const nlohmann::json& row) {
    if (!activeGzip) return;
    std::string type = row["type"];
    if (IsDuplicate(type, row)) return;

    std::string line = row.dump() + "\n";
    gzwrite(activeGzip, line.c_str(), (unsigned int)line.size());

    // Flush and close on SEND race event
    if (type == "race_event" && row["code"] == "SEND") {
        CloseActiveStream();
    }
}

// Parsing implementation for status details
nlohmann::json ParseStatus(const uint8_t* data, int base, const PacketHeader& hdr) {
    int o = base;
    o += 2;
    uint8_t fuelMix = data[o++];
    uint8_t brakeBias = data[o++];
    o += 1;
    float fuelKg = ReadFloat(data, o); o += 4;
    o += 4;
    float fuelLaps = ReadFloat(data, o); o += 4;
    o += 2;
    o += 2;
    o += 1;
    bool drsAllowed = data[o++] != 0;
    o += 2;
    uint8_t tyreCompound = data[o++];
    uint8_t visualCompound = data[o++];
    uint8_t tyreAgeLaps = data[o++];
    o += 1;
    float enginePowerICE = ReadFloat(data, o); o += 4;
    float enginePowerMGUK = ReadFloat(data, o); o += 4;
    float ersJ = ReadFloat(data, o); o += 4;
    uint8_t ersMode = data[o++];
    float ersHarvestedMGUK = ReadFloat(data, o); o += 4;
    float ersHarvestedMGUH = ReadFloat(data, o); o += 4;
    float ersDeployedJ = ReadFloat(data, o);

    double ersPct = Round1(ersJ / 40000.0);

    return {
        {"fuel_mix", fuelMix},
        {"front_brake_bias", brakeBias},
        {"fuel_kg", Round2(fuelKg)},
        {"fuel_laps", Round1(fuelLaps)},
        {"drs_allowed", drsAllowed},
        {"tyre_compound", tyreCompound},
        {"visual_compound", visualCompound},
        {"tyre_age_laps", tyreAgeLaps},
        {"ers_j", (int)std::round(ersJ)},
        {"ers_pct", ersPct},
        {"ers_mode", ersMode},
        {"ers_deployed_j", (int)std::round(ersDeployedJ)},
        {"engine_power_ice_kw", Round1(enginePowerICE / 1000.0)},
        {"engine_power_mguk_kw", Round1(enginePowerMGUK / 1000.0)},
        {"ers_harvested_mguk_j", (int)std::round(ersHarvestedMGUK)},
        {"ers_harvested_mguh_j", (int)std::round(ersHarvestedMGUH)}
    };
}

// Central Packet Router & Parser
void ProcessPacket(const uint8_t* data, int length) {
    if (length < HEADER_SIZE) return;

    // Parse packet format
    uint16_t format = ReadUInt16(data, 0);
    if (format != 2024 && format != 2025) return;

    PacketHeader hdr;
    hdr.packetFormat = format;
    hdr.packetId = data[6];
    hdr.sessionTime = ReadFloat(data, 15);
    hdr.overallFrameId = ReadUInt32(data, 23);
    hdr.playerCarIndex = data[27];

    // Rate Limiting
    uint64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    if (FRAME_SAMPLED.find(hdr.packetId) != FRAME_SAMPLED.end()) {
        auto it = lastFrameId.find(hdr.packetId);
        if (it != lastFrameId.end() && it->second == hdr.overallFrameId) return;
        lastFrameId[hdr.packetId] = hdr.overallFrameId;
    } else {
        auto itLimit = SLOW_RATE_MS.find(hdr.packetId);
        int rateMs = 500;
        if (itLimit != SLOW_RATE_MS.end()) {
            rateMs = itLimit->second;
        }
        if (rateMs > 0) {
            auto itTime = lastSlowMs.find(hdr.packetId);
            if (itTime != lastSlowMs.end() && (nowMs - itTime->second) < (uint64_t)rateMs) return;
            lastSlowMs[hdr.packetId] = nowMs;
        }
    }

    std::lock_guard<std::mutex> lock(streamMutex);

    switch (hdr.packetId) {
        case PID_SESSION: {
            if (length < 708) return;
            uint8_t weather = data[29];
            int8_t trackTemp = ReadInt8(data, 30);
            int8_t airTemp = ReadInt8(data, 31);
            uint8_t totalLaps = data[32];
            uint16_t trackLengthM = ReadUInt16(data, 33);
            uint8_t sessionType = data[35];
            int8_t trackId = ReadInt8(data, 36);
            uint16_t sessionTimeLeft = ReadUInt16(data, 38);
            uint16_t sessionDuration = ReadUInt16(data, 40);
            uint8_t pitSpeedLimit = data[42];
            uint8_t numMarshalZones = data[47];

            nlohmann::json marshalZones = nlohmann::json::array();
            for (int i = 0; i < numMarshalZones && i < 21; ++i) {
                int o = 48 + i * 5;
                marshalZones.push_back({{"zone_start", ReadFloat(data, o)}, {"flag", ReadInt8(data, o + 4)}});
            }

            uint8_t safetyCarStatus = data[153];
            uint8_t numForecastSamples = data[155];

            nlohmann::json weatherForecast = nlohmann::json::array();
            for (int i = 0; i < numForecastSamples && i < 64; ++i) {
                int o = 156 + i * 8;
                weatherForecast.push_back({
                    {"time_offset", ReadUInt8(data, o + 1)},
                    {"weather", ReadUInt8(data, o + 2)},
                    {"rain_percentage", ReadUInt8(data, o + 7)}
                });
            }

            uint8_t forecastAccuracy = data[668];
            uint8_t aiDifficulty = data[669];
            uint8_t pitStopWindowIdealLap = data[682];
            uint8_t pitStopWindowLatestLap = data[683];
            uint8_t pitStopRejoinPosition = data[684];
            uint32_t timeOfDay = ReadUInt32(data, 696);
            uint8_t numSafetyCarPeriods = data[705];
            uint8_t numVirtualScPeriods = data[706];
            uint8_t numRedFlagPeriods = data[707];

            // Auto start stream if needed
            if (trackId != currentTrackId || sessionType != currentSessionType || !activeGzip) {
                StartNewStream(trackId, sessionType, format);
            }

            nlohmann::json row = {
                {"type", "session"}, {"ts", GetISOTimestamp()},
                {"weather", weather}, {"track_temp", trackTemp}, {"air_temp", airTemp},
                {"track_length_m", trackLengthM},
                {"track_id", trackId}, {"session_type", sessionType},
                {"total_laps", totalLaps}, {"session_time_left", sessionTimeLeft},
                {"session_duration", sessionDuration}, {"pit_speed_limit", pitSpeedLimit},
                {"pit_stop_window_ideal_lap", pitStopWindowIdealLap},
                {"pit_stop_window_latest_lap", pitStopWindowLatestLap},
                {"pit_stop_rejoin_position", pitStopRejoinPosition},
                {"num_marshal_zones", numMarshalZones}, {"marshal_zones", marshalZones},
                {"weather_forecast_samples", weatherForecast},
                {"safety_car_status", safetyCarStatus},
                {"forecast_accuracy", forecastAccuracy},
                {"ai_difficulty", aiDifficulty},
                {"time_of_day", timeOfDay},
                {"num_safety_car_periods", numSafetyCarPeriods},
                {"num_virtual_sc_periods", numVirtualScPeriods},
                {"num_red_flag_periods", numRedFlagPeriods}
            };
            RecordRow(row);
            break;
        }
        case PID_MOTION: {
            int motionSize = 60;
            int base = HEADER_SIZE + hdr.playerCarIndex * motionSize;
            if (length < base + motionSize) return;

            int o = base + 36;
            float gLat = ReadFloat(data, o);
            float gLong = ReadFloat(data, o + 4);
            float gVert = ReadFloat(data, o + 8);

            nlohmann::json row = {
                {"type", "motion"}, {"ts", GetISOTimestamp()}, {"session_time", hdr.sessionTime},
                {"g_lat", Round3(gLat)}, {"g_long", Round3(gLong)}, {"g_vert", Round3(gVert)}
            };
            RecordRow(row);

            // allMotion x,z positions
            if (length >= HEADER_SIZE + 22 * motionSize) {
                nlohmann::json cars = nlohmann::json::array();
                for (int i = 0; i < 22; ++i) {
                    int cBase = HEADER_SIZE + i * motionSize;
                    float x = ReadFloat(data, cBase);
                    float z = ReadFloat(data, cBase + 8);
                    cars.push_back({{"idx", i}, {"x", Round2(x)}, {"z", Round2(z)}});
                }
                nlohmann::json posRow = {
                    {"type", "positions"}, {"ts", GetISOTimestamp()},
                    {"player_idx", hdr.playerCarIndex}, {"cars", cars}
                };
                RecordRow(posRow);
            }
            break;
        }
        case PID_LAP_DATA: {
            int lapSize = 57;
            int base = HEADER_SIZE + hdr.playerCarIndex * lapSize;
            if (length < base + lapSize) return;

            // player lap
            int o = base;
            uint32_t lastLap = ReadUInt32(data, o); o += 4;
            uint32_t curLap = ReadUInt32(data, o); o += 4;
            uint16_t s1H = ReadUInt16(data, o); o += 2;
            uint8_t s1M = data[o++];
            uint16_t s2H = ReadUInt16(data, o); o += 2;
            uint8_t s2M = data[o++];
            o += 3; // skip
            o += 3; // skip
            o += 12; // skip
            uint8_t position = data[o++];
            uint8_t lapNum = data[o++];
            uint8_t pitStatus = data[o++];
            uint8_t numPits = data[o++];
            uint8_t sector = data[o++];
            bool invalid = data[o++] != 0;
            uint8_t penaltiesS = data[o];

            nlohmann::json row = {
                {"type", "lap"}, {"ts", GetISOTimestamp()}, {"session_time", hdr.sessionTime},
                {"last_lap_ms", lastLap}, {"current_lap_ms", curLap},
                {"s1_ms", s1M * 60000 + s1H}, {"s2_ms", s2M * 60000 + s2H},
                {"position", position}, {"lap_num", lapNum},
                {"pit_status", pitStatus}, {"num_pit_stops", numPits},
                {"sector", sector}, {"lap_invalid", invalid}, {"penalties_s", penaltiesS}
            };
            RecordRow(row);

            // allCars timing
            if (length >= HEADER_SIZE + 22 * lapSize) {
                nlohmann::json cars = nlohmann::json::array();
                for (int i = 0; i < 22; ++i) {
                    int cBase = HEADER_SIZE + i * lapSize;
                    int co = cBase;
                    uint32_t clast = ReadUInt32(data, co); co += 4;
                    uint32_t ccur = ReadUInt32(data, co); co += 4;
                    uint16_t cs1H = ReadUInt16(data, co); co += 2;
                    uint8_t cs1M = data[co++];
                    uint16_t cs2H = ReadUInt16(data, co); co += 2;
                    uint8_t cs2M = data[co++];
                    co += 3;
                    uint16_t cgapH = ReadUInt16(data, co); co += 2;
                    uint8_t cgapM = data[co++];
                    co += 12;
                    uint8_t cpos = data[co++];
                    uint8_t clap = data[co++];
                    uint8_t cpit = data[co++];
                    co += 1; // numPits skip
                    uint8_t csect = data[co++];
                    bool cinvalid = data[co++] != 0;
                    uint8_t cpen = data[co++];
                    co += 2; // skip
                    uint8_t cdt = data[co++];
                    uint8_t csg = data[co++];
                    co += 1;
                    uint8_t cdriverStat = data[co++];
                    uint8_t cresultStat = data[co];

                    cars.push_back({
                        {"idx", i}, {"position", cpos}, {"lap_num", clap},
                        {"current_lap_ms", ccur}, {"last_lap_ms", clast},
                        {"s1_ms", cs1M * 60000 + cs1H}, {"s2_ms", cs2M * 60000 + cs2H},
                        {"gap_ms", cgapM * 60000 + cgapH}, {"pit_status", cpit},
                        {"lap_invalid", cinvalid}, {"penalties_s", cpen},
                        {"num_dt_pens", cdt}, {"num_sg_pens", csg}, {"sector", csect},
                        {"result_status", cresultStat}, {"driver_status", cdriverStat}
                    });
                }
                nlohmann::json timingRow = {
                    {"type", "timing"}, {"ts", GetISOTimestamp()}, {"session_time", hdr.sessionTime},
                    {"player_idx", hdr.playerCarIndex}, {"cars", cars}
                };
                RecordRow(timingRow);
            }
            break;
        }
        case PID_CAR_TEL: {
            int telSize = 60;
            int base = HEADER_SIZE + hdr.playerCarIndex * telSize;
            if (length < base + telSize) return;

            int o = base;
            uint16_t speed = ReadUInt16(data, o); o += 2;
            float throt = ReadFloat(data, o); o += 4;
            float steer = ReadFloat(data, o); o += 4;
            float brake = ReadFloat(data, o); o += 4;
            o += 1; // skip clutch
            int8_t gear = ReadInt8(data, o); o += 1;
            uint16_t rpm = ReadUInt16(data, o); o += 2;
            uint8_t drs = data[o++];
            o += 1; // skip revLightsPct
            o += 2; // skip revLightsBitField
            uint16_t btRL = ReadUInt16(data, o); o += 2;
            uint16_t btRR = ReadUInt16(data, o); o += 2;
            uint16_t btFL = ReadUInt16(data, o); o += 2;
            uint16_t btFR = ReadUInt16(data, o); o += 2;
            uint8_t stRL = data[o++]; uint8_t stRR = data[o++]; uint8_t stFL = data[o++]; uint8_t stFR = data[o++];
            uint8_t itRL = data[o++]; uint8_t itRR = data[o++]; uint8_t itFL = data[o++]; uint8_t itFR = data[o++];
            uint16_t engT = ReadUInt16(data, o);

            nlohmann::json row = {
                {"type", "telemetry"}, {"ts", GetISOTimestamp()}, {"session_time", hdr.sessionTime},
                {"speed_kph", speed}, {"rpm", rpm}, {"gear", gear},
                {"throttle", throt}, {"brake", brake}, {"steering", Round4(steer)}, {"drs", drs},
                {"tyre_temp_surface_rl", stRL}, {"tyre_temp_surface_rr", stRR},
                {"tyre_temp_surface_fl", stFL}, {"tyre_temp_surface_fr", stFR},
                {"tyre_temp_inner_rl", itRL}, {"tyre_temp_inner_rr", itRR},
                {"tyre_temp_inner_fl", itFL}, {"tyre_temp_inner_fr", itFR},
                {"brake_temp_rl", btRL}, {"brake_temp_rr", btRR},
                {"brake_temp_fl", btFL}, {"brake_temp_fr", btFR},
                {"engine_temp", engT}
            };
            RecordRow(row);
            break;
        }
        case PID_CAR_STATUS: {
            int statusSize = 55;
            int base = HEADER_SIZE + hdr.playerCarIndex * statusSize;
            if (length < base + statusSize) return;

            nlohmann::json row = ParseStatus(data, base, hdr);
            row["type"] = "status";
            row["ts"] = GetISOTimestamp();
            row["session_time"] = hdr.sessionTime;
            RecordRow(row);

            // allStatus
            if (length >= HEADER_SIZE + 22 * statusSize) {
                nlohmann::json cars = nlohmann::json::array();
                for (int i = 0; i < 22; ++i) {
                    nlohmann::json cStat = ParseStatus(data, HEADER_SIZE + i * statusSize, hdr);
                    cStat["idx"] = i;
                    cars.push_back(cStat);
                }
                nlohmann::json allRow = {
                    {"type", "all_status"}, {"ts", GetISOTimestamp()},
                    {"session_time", hdr.sessionTime}, {"cars", cars}
                };
                RecordRow(allRow);
            }
            break;
        }
        case PID_CAR_DAMAGE: {
            if (format == 2025) {
                int damageSize = 46;
                int base = HEADER_SIZE + hdr.playerCarIndex * damageSize;
                if (length < base + damageSize) return;

                int o = base;
                float wRL = ReadFloat(data, o); o += 4;
                float wRR = ReadFloat(data, o); o += 4;
                float wFL = ReadFloat(data, o); o += 4;
                float wFR = ReadFloat(data, o); o += 4;
                o += 8; // skip
                uint8_t blRL = data[o++]; uint8_t blRR = data[o++]; uint8_t blFL = data[o++]; uint8_t blFR = data[o++];
                uint8_t wingFL = data[o++]; uint8_t wingFR = data[o++]; uint8_t wingRear = data[o++];
                uint8_t floorDmg = data[o++]; uint8_t carpetDmg = data[o++]; uint8_t diffuserDmg = data[o++];
                uint8_t gearboxDmg = data[o++]; uint8_t engineDmg = data[o++];

                nlohmann::json row = {
                    {"type", "damage"}, {"ts", GetISOTimestamp()}, {"session_time", hdr.sessionTime},
                    {"tyre_wear_rl", Round1(wRL)}, {"tyre_wear_rr", Round1(wRR)},
                    {"tyre_wear_fl", Round1(wFL)}, {"tyre_wear_fr", Round1(wFR)},
                    {"blisters_rl", blRL}, {"blisters_rr", blRR}, {"blisters_fl", blFL}, {"blisters_fr", blFR},
                    {"wing_fl", wingFL}, {"wing_fr", wingFR}, {"wing_rear", wingRear},
                    {"floor_damage", floorDmg}, {"carpet_damage", carpetDmg},
                    {"diffuser_damage", diffuserDmg}, {"gearbox_damage", gearboxDmg}, {"engine_damage", engineDmg}
                };
                RecordRow(row);
            } else { // F1 2024
                int damageSize = 42;
                int base = HEADER_SIZE + hdr.playerCarIndex * damageSize;
                if (length < base + damageSize) return;

                int o = base;
                float wRL = ReadFloat(data, o); o += 4;
                float wRR = ReadFloat(data, o); o += 4;
                float wFL = ReadFloat(data, o); o += 4;
                float wFR = ReadFloat(data, o); o += 4;
                o += 8; // skip
                uint8_t wingFL = data[o++]; uint8_t wingFR = data[o++]; uint8_t wingRear = data[o++];
                uint8_t floorDmg = data[o++]; uint8_t carpetDmg = data[o++]; uint8_t diffuserDmg = data[o++];
                uint8_t gearboxDmg = data[o++]; uint8_t engineDmg = data[o++];

                nlohmann::json row = {
                    {"type", "damage"}, {"ts", GetISOTimestamp()}, {"session_time", hdr.sessionTime},
                    {"tyre_wear_rl", Round1(wRL)}, {"tyre_wear_rr", Round1(wRR)},
                    {"tyre_wear_fl", Round1(wFL)}, {"tyre_wear_fr", Round1(wFR)},
                    {"blisters_rl", 0}, {"blisters_rr", 0}, {"blisters_fl", 0}, {"blisters_fr", 0},
                    {"wing_fl", wingFL}, {"wing_fr", wingFR}, {"wing_rear", wingRear},
                    {"floor_damage", floorDmg}, {"carpet_damage", carpetDmg},
                    {"diffuser_damage", diffuserDmg}, {"gearbox_damage", gearboxDmg}, {"engine_damage", engineDmg}
                };
                RecordRow(row);
            }
            break;
        }
        case PID_PARTICIPANTS: {
            if (format == 2025) {
                int partSize = 57;
                if (length < HEADER_SIZE + 1 + 22 * partSize) return;
                nlohmann::json drivers = nlohmann::json::array();
                for (int i = 0; i < 22; ++i) {
                    int o = HEADER_SIZE + 1 + i * partSize;
                    bool ai = data[o] != 0; o += 1;
                    o += 2;
                    uint8_t teamId = data[o]; o += 1;
                    o += 1;
                    uint8_t raceNum = data[o]; o += 1;
                    o += 1;
                    int nameStart = o;
                    std::string name = ReadString(data, nameStart, 32);
                    if (name.empty()) continue;
                    uint8_t numColors = data[nameStart + 37];
                    uint8_t r = data[nameStart + 38];
                    uint8_t g = data[nameStart + 39];
                    uint8_t b = data[nameStart + 40];
                    char hexColor[16];
                    if (numColors > 0) {
                        sprintf_s(hexColor, "#%02x%02x%02x", r, g, b);
                    } else {
                        strcpy_s(hexColor, "#8e8e8e");
                    }
                    drivers.push_back({
                        {"idx", i}, {"name", name}, {"team_id", teamId},
                        {"race_number", raceNum}, {"ai", ai}, {"livery_color", hexColor}
                    });
                }
                nlohmann::json row = {{"type", "participants"}, {"drivers", drivers}};
                RecordRow(row);
            } else { // F1 2024
                int partSize = 60;
                if (length < HEADER_SIZE + 1 + 22 * partSize) return;
                nlohmann::json drivers = nlohmann::json::array();
                for (int i = 0; i < 22; ++i) {
                    int o = HEADER_SIZE + 1 + i * partSize;
                    bool ai = data[o] != 0; o += 1;
                    o += 2;
                    uint8_t teamId = data[o]; o += 1;
                    o += 1;
                    uint8_t raceNum = data[o]; o += 1;
                    o += 1;
                    int nameStart = o;
                    std::string name = ReadString(data, nameStart, 48);
                    if (name.empty()) continue;
                    std::string liveryColor = "#8e8e8e";
                    auto it = F1_24_TEAM_COLORS.find(teamId);
                    if (it != F1_24_TEAM_COLORS.end()) {
                        liveryColor = it->second;
                    }
                    drivers.push_back({
                        {"idx", i}, {"name", name}, {"team_id", teamId},
                        {"race_number", raceNum}, {"ai", ai}, {"livery_color", liveryColor}
                    });
                }
                nlohmann::json row = {{"type", "participants"}, {"drivers", drivers}};
                RecordRow(row);
            }
            break;
        }
        case PID_EVENT: {
            if (length < HEADER_SIZE + 4) return;
            std::string code(reinterpret_cast<const char*>(data + HEADER_SIZE), 4);
            nlohmann::json base = {
                {"type", "race_event"},
                {"ts", GetISOTimestamp()},
                {"session_time", hdr.sessionTime},
                {"code", code}
            };
            int o = HEADER_SIZE + 4;
            if (code == "FTLP") {
                if (length < o + 5) return;
                uint8_t vehicleIdx = data[o];
                float lapTimeS = Round3(ReadFloat(data, o + 1));
                nlohmann::json fastest = {
                    {"type", "fastest_lap"},
                    {"ts", GetISOTimestamp()},
                    {"car_idx", vehicleIdx},
                    {"lap_time_s", lapTimeS}
                };
                nlohmann::json ev = base;
                ev["car_idx"] = vehicleIdx;
                ev["lap_time_s"] = lapTimeS;
                RecordRow(fastest);
                RecordRow(ev);
            } else if (code == "DRSE" || code == "DRSD" || code == "RDFL" || code == "CHQF" ||
                       code == "LGOT" || code == "SSTA" || code == "SEND") {
                RecordRow(base);
            } else if (code == "SCAR") {
                if (length < o + 2) return;
                uint8_t scType = data[o];
                uint8_t evType = data[o + 1];
                if (scType == 0) return;
                nlohmann::json ev = base;
                ev["safety_car_type"] = scType;
                ev["event_type"] = evType;
                RecordRow(ev);
            } else if (code == "RTMT" || code == "RCWN") {
                if (length < o + 1) return;
                nlohmann::json ev = base;
                ev["car_idx"] = data[o];
                RecordRow(ev);
            } else if (code == "PENA") {
                if (length < o + 7) return;
                uint8_t penType = data[o];
                uint8_t infringementType = data[o + 1];
                uint8_t vehicleIdx = data[o + 2];
                uint8_t timeS = data[o + 4];
                nlohmann::json ev = base;
                ev["car_idx"] = vehicleIdx;
                ev["penalty_type"] = penType;
                ev["infringement_type"] = infringementType;
                ev["penalty_time_s"] = timeS;
                RecordRow(ev);
            } else if (code == "DTSV" || code == "SGSV") {
                if (length < o + 1) return;
                nlohmann::json ev = base;
                ev["car_idx"] = data[o];
                RecordRow(ev);
            }
            break;
        }
        case PID_SESSION_HISTORY: {
            if (length < HEADER_SIZE + 7) return;
            uint8_t carIdx = data[HEADER_SIZE];
            uint8_t bestLapNum = data[HEADER_SIZE + 3];
            if (bestLapNum == 0) return;

            int lapOff = HEADER_SIZE + 7 + (bestLapNum - 1) * 14;
            if (length < lapOff + 14) return;

            uint8_t lapValidBitFlags = data[lapOff + 13];
            if ((lapValidBitFlags & 0x01) == 0) return;

            uint32_t bestLapTimeMs = ReadUInt32(data, lapOff);
            nlohmann::json row = {
                {"type", "session_history_fastest"},
                {"ts", GetISOTimestamp()},
                {"car_idx", carIdx},
                {"best_lap_time_ms", bestLapTimeMs}
            };
            RecordRow(row);
            break;
        }
        case PID_TYRE_SETS: {
            if (length < 231) return;
            uint8_t carIdx = data[HEADER_SIZE];
            if (carIdx != hdr.playerCarIndex) return;

            nlohmann::json sets = nlohmann::json::array();
            for (int i = 0; i < 20; ++i) {
                int o = HEADER_SIZE + 1 + i * 10;
                sets.push_back({
                    {"idx", i},
                    {"actual_compound", data[o]},
                    {"visual_compound", data[o + 1]},
                    {"wear", data[o + 2]},
                    {"available", data[o + 3] == 1},
                    {"recommended_session", data[o + 4]},
                    {"life_span", data[o + 5]},
                    {"usable_life", data[o + 6]},
                    {"lap_delta_ms", ReadInt16(data, o + 7)},
                    {"fitted", data[o + 9] == 1}
                });
            }
            uint8_t fittedIdx = data[230];
            nlohmann::json row = {
                {"type", "tyre_sets"}, {"ts", GetISOTimestamp()}, {"session_time", hdr.sessionTime},
                {"sets", sets}, {"fitted_idx", fittedIdx}
            };
            RecordRow(row);
            break;
        }
        case PID_MOTION_EX: {
            if (length < 225) return;
            float frontAero = ReadFloat(data, 217);
            float rearAero = ReadFloat(data, 221);

            nlohmann::json row = {
                {"type", "motion_ex"}, {"ts", GetISOTimestamp()}, {"session_time", hdr.sessionTime},
                {"front_aero_height_mm", Round2(frontAero * 1000.0)},
                {"rear_aero_height_mm", Round2(rearAero * 1000.0)}
            };
            RecordRow(row);
            break;
        }
    }
}

// Background receiver socket thread function
void ReceiverThreadFunc() {
    // Initialize Winsock
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        return;
    }

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        WSACleanup();
        return;
    }

    BOOL opt = TRUE;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    DWORD timeout = 100; // 100ms
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));

    sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(20777);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        closesocket(sock);
        WSACleanup();
        MessageBoxW(hMainWindow, L"Failed to bind to UDP port 20777. Is another telemetry tool or Track-N-Race open?", L"UDP Error", MB_OK | MB_ICONERROR);
        return;
    }

    uint8_t buffer[8192];
    sockaddr_in clientAddr;
    int clientAddrLen = sizeof(clientAddr);

    while (!shouldExit) {
        int bytesReceived = recvfrom(sock, (char*)buffer, sizeof(buffer), 0, (sockaddr*)&clientAddr, &clientAddrLen);
        if (bytesReceived > 0) {
            if (isRecording) {
                ProcessPacket(buffer, bytesReceived);
            }
        }
    }

    // Clean up active stream if still open
    {
        std::lock_guard<std::mutex> lock(streamMutex);
        CloseActiveStream();
    }

    closesocket(sock);
    WSACleanup();
}

// Main Window Procedure
LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_CREATE: {
            hMainWindow = hwnd; // Set global before theme updates
            
            // Status Static
            hLblStatus = CreateWindowW(L"STATIC", L"Status: Idle", WS_VISIBLE | WS_CHILD, 20, 20, 440, 25, hwnd, NULL, NULL, NULL);
            // Directory Static (supports wrapping)
            hLblDirectory = CreateWindowW(L"STATIC", L"No directory selected.", WS_VISIBLE | WS_CHILD, 20, 50, 440, 50, hwnd, NULL, NULL, NULL);
            // Buttons
            hBtnDirectory = CreateWindowW(L"BUTTON", L"Select Directory", WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON, 20, 110, 210, 35, hwnd, (HMENU)1, NULL, NULL);
            hBtnToggle = CreateWindowW(L"BUTTON", L"Start Recording", WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON, 250, 110, 210, 35, hwnd, (HMENU)2, NULL, NULL);

            // Modern font
            HFONT hFont = CreateFontW(18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
            HFONT hFontBold = CreateFontW(18, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
            SendMessage(hLblStatus, WM_SETFONT, (WPARAM)hFontBold, TRUE);
            SendMessage(hLblDirectory, WM_SETFONT, (WPARAM)hFont, TRUE);
            SendMessage(hBtnDirectory, WM_SETFONT, (WPARAM)hFont, TRUE);
            SendMessage(hBtnToggle, WM_SETFONT, (WPARAM)hFont, TRUE);

            // Apply initial theme layout & styling
            UpdateThemeColors();
            return 0;
        }

        case WM_COMMAND: {
            if (LOWORD(wParam) == 1) {
                SelectDirectory();
            } else if (LOWORD(wParam) == 2) {
                if (outputDirectory.empty()) {
                    MessageBoxW(hwnd, L"Please select an output directory first.", L"Error", MB_OK | MB_ICONERROR);
                } else {
                    isRecording = !isRecording;
                    if (isRecording) {
                        SetWindowTextW(hBtnToggle, L"Stop Recording");
                        SetWindowTextW(hLblStatus, L"Status: Waiting for game data...");
                    } else {
                        std::lock_guard<std::mutex> lock(streamMutex);
                        CloseActiveStream();
                        SetWindowTextW(hBtnToggle, L"Start Recording");
                        SetWindowTextW(hLblStatus, L"Status: Idle");
                    }
                }
            }
            return 0;
        }

        case WM_CTLCOLORSTATIC: {
            HDC hdc = (HDC)wParam;
            SetTextColor(hdc, textColor);
            SetBkColor(hdc, bgColor);
            return (INT_PTR)hBgBrush;
        }

        case WM_SETTINGCHANGE: {
            if (lParam && wcscmp((const wchar_t*)lParam, L"Registry") == 0) {
                UpdateThemeColors();
            }
            return 0;
        }

        case WM_DESTROY: {
            shouldExit = true;
            if (receiverThread.joinable()) {
                receiverThread.join();
            }
            if (hBgBrush) {
                DeleteObject(hBgBrush);
            }
            PostQuitMessage(0);
            return 0;
        }
    }
    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, PWSTR pCmdLine, int nCmdShow) {
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    const wchar_t CLASS_NAME[]  = L"RecorderWindowClass";
    
    WNDCLASSW wc = { };
    wc.lpfnWndProc   = WindowProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hbrBackground = NULL; // Prevent default background repaint to avoid flicker and override properly

    RegisterClassW(&wc);

    hMainWindow = CreateWindowExW(
        0, CLASS_NAME, L"TNRD Background Recorder",
        WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX, // Non-resizable
        CW_USEDEFAULT, CW_USEDEFAULT, 500, 210,
        NULL, NULL, hInstance, NULL
    );

    if (hMainWindow == NULL) return 0;

    ShowWindow(hMainWindow, nCmdShow);

    // Start background receiver socket
    receiverThread = std::thread(ReceiverThreadFunc);

    MSG msg = { };
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    CoUninitialize();
    return 0;
}
