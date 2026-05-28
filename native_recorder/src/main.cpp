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
#include <shellapi.h>
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
#include <algorithm>
#include <nlohmann/json.hpp>
#include <zlib.h>

#include "protocol.h"
#include "f1_24.h"
#include "f1_25.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(linker,"\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

// UI Elements
HWND hMainWindow = NULL;
HWND hBtnDirectory = NULL;
HWND hBtnToggle = NULL;
HWND hBtnSwitch = NULL;
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

float lastSessionTime = -1.0f;

const float BUFFER_WINDOW_S = 30.0f;

struct BufferEntry {
    std::string line;
    float sessionTime;
};
std::vector<BufferEntry> rollingBuffer;

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

// Deduplication State
const std::unordered_set<std::string> DEDUPE_TYPES = {
    "session", "tyre_sets", "participants", "all_status", "status", "timing", "damage"
};
std::unordered_map<std::string, std::string> dedupeCache;

// Rate limiting state
std::unordered_map<int, uint32_t> lastFrameId;
std::unordered_map<int, uint64_t> lastSlowMs;

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

bool isMicaSupported = false;

typedef void (WINAPI* RtlGetVersionPtr)(PRTL_OSVERSIONINFOW);

bool CheckMicaSupport() {
    HMODULE hMod = GetModuleHandleW(L"ntdll.dll");
    if (hMod) {
        RtlGetVersionPtr pRtlGetVersion = (RtlGetVersionPtr)GetProcAddress(hMod, "RtlGetVersion");
        if (pRtlGetVersion) {
            RTL_OSVERSIONINFOW osvi = { 0 };
            osvi.dwOSVersionInfoSize = sizeof(osvi);
            pRtlGetVersion(&osvi);
            return osvi.dwBuildNumber >= 22000;
        }
    }
    return false;
}

void SetLabelText(HWND hLabel, const wchar_t* text) {
    if (!hLabel) return;
    if (isMicaSupported && hMainWindow) {
        RECT rc;
        GetWindowRect(hLabel, &rc);
        MapWindowPoints(HWND_DESKTOP, hMainWindow, (LPPOINT)&rc, 2);
        InvalidateRect(hMainWindow, &rc, TRUE);
    }
    SetWindowTextW(hLabel, text);
}

void UpdateThemeColors() {
    bool isDark = IsSystemDarkMode();
    if (hBgBrush) {
        DeleteObject(hBgBrush);
    }
    
    if (isMicaSupported) {
        // When Mica is active, the window background must be black for DWM transparency to work
        bgColor = RGB(0, 0, 0);
        textColor = isDark ? RGB(255, 255, 255) : RGB(0, 0, 0);
        hBgBrush = CreateSolidBrush(bgColor);
    } else {
        if (isDark) {
            bgColor = RGB(32, 32, 32);
            textColor = RGB(255, 255, 255);
            hBgBrush = CreateSolidBrush(bgColor);
        } else {
            bgColor = GetSysColor(COLOR_WINDOW);
            textColor = GetSysColor(COLOR_WINDOWTEXT);
            hBgBrush = CreateSolidBrush(bgColor);
        }
    }
    
    if (hMainWindow) {
        // Dynamic class background swap
        SetClassLongPtrW(hMainWindow, GCLP_HBRBACKGROUND, (LONG_PTR)hBgBrush);

        // Apply dark title bar (DWM)
        BOOL useDark = isDark ? TRUE : FALSE;
        DwmSetWindowAttribute(hMainWindow, DWMWA_USE_IMMERSIVE_DARK_MODE, &useDark, sizeof(useDark));
        
        if (isMicaSupported) {
            // Extend the frame into the client area
            MARGINS margins = { -1 };
            DwmExtendFrameIntoClientArea(hMainWindow, &margins);

            // Set Mica backdrop
            DWORD buildNumber = 0;
            HMODULE hMod = GetModuleHandleW(L"ntdll.dll");
            if (hMod) {
                RtlGetVersionPtr pRtlGetVersion = (RtlGetVersionPtr)GetProcAddress(hMod, "RtlGetVersion");
                if (pRtlGetVersion) {
                    RTL_OSVERSIONINFOW osvi = { 0 };
                    osvi.dwOSVersionInfoSize = sizeof(osvi);
                    pRtlGetVersion(&osvi);
                    buildNumber = osvi.dwBuildNumber;
                }
            }

            if (buildNumber >= 22621) {
                // Windows 11 22H2+
                #ifndef DWMWA_SYSTEMBACKDROP_TYPE
                #define DWMWA_SYSTEMBACKDROP_TYPE 38
                #endif
                int backdrop = 2; // DWMSBT_MAINWINDOW (Standard Mica)
                DwmSetWindowAttribute(hMainWindow, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop));
            } else if (buildNumber >= 22000) {
                // Windows 11 21H2 (original release)
                #ifndef DWMWA_MICA_EFFECT
                #define DWMWA_MICA_EFFECT 1029
                #endif
                BOOL enableMica = TRUE;
                DwmSetWindowAttribute(hMainWindow, DWMWA_MICA_EFFECT, &enableMica, sizeof(enableMica));
            }
        }
        
        // Native explorer light/dark control themes
        const wchar_t* themeName = isDark ? L"DarkMode_Explorer" : L"Explorer";
        if (hBtnDirectory) SetWindowTheme(hBtnDirectory, themeName, NULL);
        if (hBtnToggle) SetWindowTheme(hBtnToggle, themeName, NULL);
        if (hBtnSwitch) SetWindowTheme(hBtnSwitch, themeName, NULL);

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
                    SetLabelText(hLblDirectory, pszFilePath);
                    CoTaskMemFree(pszFilePath);
                }
                psi->Release();
            }
        }
        pfd->Release();
    }
}

void FlushBufferToDisk(const std::vector<BufferEntry>& entries);

// Gzip stream close
void CloseActiveStream() {
    if (activeGzip) {
        FlushBufferToDisk(rollingBuffer);
        gzclose(activeGzip);
        activeGzip = NULL;
        SetLabelText(hLblStatus, L"Status: Idle");
    }
    rollingBuffer.clear();
    currentTrackId = -1;
    currentSessionType = -1;
    activeGzipPath = L"";
    lastSessionTime = -1.0f;
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
        lastSessionTime = -1.0f;

        // UI Label update
        std::wstring statusStr = L"Recording: " + filenameW.substr(0, 30) + L"...";
        SetLabelText(hLblStatus, statusStr.c_str());
    }
}

void FlushBufferToDisk(const std::vector<BufferEntry>& entries) {
    if (!activeGzip || entries.empty()) return;
    for (const auto& entry : entries) {
        gzwrite(activeGzip, entry.line.c_str(), (unsigned int)entry.line.size());
    }
}

void FlushOldBufferEntries() {
    if (lastSessionTime < 0.0f || rollingBuffer.empty()) return;
    float cutoff = lastSessionTime - BUFFER_WINDOW_S;
    size_t flushIdx = 0;
    while (flushIdx < rollingBuffer.size() && rollingBuffer[flushIdx].sessionTime < cutoff) {
        flushIdx++;
    }
    if (flushIdx > 0) {
        FlushBufferToDisk(std::vector<BufferEntry>(rollingBuffer.begin(), rollingBuffer.begin() + flushIdx));
        rollingBuffer.erase(rollingBuffer.begin(), rollingBuffer.begin() + flushIdx);
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

// Gzip Session Rewind Truncation Engine
void TruncateTimeline(float newSessionTime) {
    float bufferStart = rollingBuffer.empty() ? INFINITY : rollingBuffer[0].sessionTime;

    if (newSessionTime >= bufferStart) {
        // Rewind is within the buffer window — trim in memory, no file I/O needed
        rollingBuffer.erase(
            std::remove_if(rollingBuffer.begin(), rollingBuffer.end(),
                [newSessionTime](const BufferEntry& e) { return e.sessionTime > newSessionTime; }),
            rollingBuffer.end()
        );
    } else {
        // Rewind goes past the buffer — drop the entire buffer and truncate the on-disk portion
        rollingBuffer.clear();

        if (!activeGzipPath.empty() && activeGzip) {
            gzclose(activeGzip);
            activeGzip = NULL;

            std::vector<std::string> retainedLines;
            gzFile infile = gzopen_w(activeGzipPath.c_str(), "rb");
            if (infile) {
                char buf[16384];
                while (gzgets(infile, buf, sizeof(buf)) != NULL) {
                    std::string line(buf);
                    try {
                        nlohmann::json j = nlohmann::json::parse(line);
                        if (j.contains("session_time")) {
                            if (j["session_time"].get<float>() <= newSessionTime)
                                retainedLines.push_back(line);
                        } else {
                            retainedLines.push_back(line);
                        }
                    } catch (...) {}
                }
                gzclose(infile);
            }

            gzFile outfile = gzopen_w(activeGzipPath.c_str(), "wb");
            if (outfile) {
                for (const auto& line : retainedLines)
                    gzwrite(outfile, line.c_str(), (unsigned int)line.size());
                gzclose(outfile);
            }

            activeGzip = gzopen_w(activeGzipPath.c_str(), "ab");
        }
    }

    dedupeCache.clear();
    lastSessionTime = newSessionTime;

    std::wstring statusMsg = L"Flashback: Resuming from " + std::to_wstring((int)newSessionTime) + L"s...";
    SetLabelText(hLblStatus, statusMsg.c_str());
}

// Writes a telemetry row to the rolling buffer
void RecordRow(const nlohmann::json& row, float sessionTime) {
    if (!activeGzip) return;
    std::string type = row["type"];
    if (IsDuplicate(type, row)) return;

    std::string line = row.dump() + "\n";
    float entryTime = (sessionTime >= 0.0f) ? sessionTime : lastSessionTime;
    rollingBuffer.push_back({line, entryTime});

    if (type == "race_event" && row["code"] == "SEND") {
        CloseActiveStream();
        return;
    }

    FlushOldBufferEntries();
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

    // Dynamic Flashback / Rewind Truncation Engine
    if (activeGzip && lastSessionTime >= 0.0f && hdr.sessionTime < lastSessionTime - 0.2f) {
        TruncateTimeline(hdr.sessionTime);
    } else if (hdr.sessionTime > lastSessionTime) {
        lastSessionTime = hdr.sessionTime;
    }

    // Auto start stream on session packet if needed
    if (hdr.packetId == PID_SESSION && length >= 708) {
        int8_t trackId = ReadInt8(data, 36);
        uint8_t sessionType = data[35];
        if (trackId != currentTrackId || sessionType != currentSessionType || !activeGzip) {
            StartNewStream(trackId, sessionType, format);
        }
    }

    if (!activeGzip) return;

    // Dispatch parser based on format dynamically
    std::vector<nlohmann::json> rows;
    std::string timestamp = GetISOTimestamp();
    
    if (format == 2024) {
        rows = F1_24::ParsePacket(data, length, hdr, timestamp);
    } else if (format == 2025) {
        rows = F1_25::ParsePacket(data, length, hdr, timestamp);
    }

    for (const auto& row : rows) {
        RecordRow(row, hdr.sessionTime);
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

// Spawns the Electron Analyzer and exits the Background Recorder
void SwitchToElectron() {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(NULL, exePath, MAX_PATH);
    std::wstring dirPath = exePath;
    size_t lastSlash = dirPath.find_last_of(L"\\/");
    if (lastSlash != std::wstring::npos) {
        dirPath = dirPath.substr(0, lastSlash);
    }

    std::wstring electronExePath = dirPath + L"\\Track N Race.exe";
    
    // Check if production executable exists
    DWORD attrib = GetFileAttributesW(electronExePath.c_str());
    if (attrib != INVALID_FILE_ATTRIBUTES && !(attrib & FILE_ATTRIBUTE_DIRECTORY)) {
        // Production mode: run Track N Race.exe
        ShellExecuteW(NULL, L"open", electronExePath.c_str(), NULL, dirPath.c_str(), SW_SHOW);
    } else {
        // Dev mode: check for package.json 3 directories up
        std::wstring parentDir = dirPath;
        for (int i = 0; i < 3; i++) {
            size_t pos = parentDir.find_last_of(L"\\/");
            if (pos != std::wstring::npos) {
                parentDir = parentDir.substr(0, pos);
            }
        }
        std::wstring packageJsonPath = parentDir + L"\\package.json";
        DWORD pkgAttrib = GetFileAttributesW(packageJsonPath.c_str());
        if (pkgAttrib != INVALID_FILE_ATTRIBUTES && !(pkgAttrib & FILE_ATTRIBUTE_DIRECTORY)) {
            // Run npm run dev in workspace root
            ShellExecuteW(NULL, L"open", L"cmd.exe", L"/c npm run dev", parentDir.c_str(), SW_SHOW);
        } else {
            MessageBoxW(hMainWindow, L"Track N Race executable not found.", L"Error", MB_OK | MB_ICONERROR);
            return;
        }
    }

    // Stop recording if active and terminate current process
    shouldExit = true;
    PostQuitMessage(0);
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
            hBtnSwitch = CreateWindowW(L"BUTTON", L"Switch to Track N Race App", WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON, 20, 160, 440, 35, hwnd, (HMENU)3, NULL, NULL);

            // Modern font
            HFONT hFont = CreateFontW(18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
            HFONT hFontBold = CreateFontW(18, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
            SendMessage(hLblStatus, WM_SETFONT, (WPARAM)hFontBold, TRUE);
            SendMessage(hLblDirectory, WM_SETFONT, (WPARAM)hFont, TRUE);
            SendMessage(hBtnDirectory, WM_SETFONT, (WPARAM)hFont, TRUE);
            SendMessage(hBtnToggle, WM_SETFONT, (WPARAM)hFont, TRUE);
            SendMessage(hBtnSwitch, WM_SETFONT, (WPARAM)hFont, TRUE);

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
                        SetLabelText(hLblStatus, L"Status: Waiting for game data...");
                    } else {
                        std::lock_guard<std::mutex> lock(streamMutex);
                        CloseActiveStream();
                        SetWindowTextW(hBtnToggle, L"Start Recording");
                        SetLabelText(hLblStatus, L"Status: Idle");
                    }
                }
            } else if (LOWORD(wParam) == 3) {
                SwitchToElectron();
            }
            return 0;
        }

        case WM_CTLCOLORSTATIC: {
            HDC hdc = (HDC)wParam;
            SetTextColor(hdc, textColor);
            if (isMicaSupported) {
                SetBkMode(hdc, TRANSPARENT);
                return (INT_PTR)GetStockObject(NULL_BRUSH);
            } else {
                SetBkColor(hdc, bgColor);
                return (INT_PTR)hBgBrush;
            }
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
    isMicaSupported = CheckMicaSupport();

    const wchar_t CLASS_NAME[]  = L"RecorderWindowClass";
    
    WNDCLASSEXW wc = { };
    wc.cbSize        = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc   = WindowProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hIcon         = LoadIconW(hInstance, MAKEINTRESOURCEW(1));
    wc.hIconSm       = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);
    wc.hbrBackground = NULL; // Prevent default background repaint to avoid flicker and override properly

    RegisterClassExW(&wc);

    hMainWindow = CreateWindowExW(
        0, CLASS_NAME, L"Track N Race Background Recorder",
        WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX, // Non-resizable
        CW_USEDEFAULT, CW_USEDEFAULT, 500, 260,
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
