#include "Attributions.h"
#include "MinimalController.h"

#include <winsock2.h>
#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <uxtheme.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.ViewManagement.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace {

constexpr wchar_t kWindowClass[] = L"TrackNRaceMinimalRecorderWindow";
constexpr wchar_t kAttributionWindowClass[] = L"TrackNRaceMinimalRecorderAttributions";
constexpr wchar_t kRegistryPath[] = L"Software\\Track N Race\\Minimal App";
constexpr UINT kSessionUpdated = WM_APP + 1;
constexpr UINT kSystemThemeChanged = WM_APP + 2;
constexpr COLORREF kDarkWindowColor = RGB(32, 32, 32);
constexpr COLORREF kDarkControlColor = RGB(43, 43, 43);
constexpr COLORREF kDarkTextColor = RGB(240, 240, 240);

enum ControlId : int {
    FolderEdit = 100,
    BrowseButton,
    ProtocolCombo,
    AddressEdit,
    PortEdit,
    ApplyButton,
    CircuitValue,
    SessionValue,
    AttributionButton,
};

std::string utf8(std::wstring_view value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                         static_cast<int>(value.size()),
                                         nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring wide(std::string_view value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                         value.data(), static_cast<int>(value.size()),
                                         nullptr, 0);
    if (size <= 0) return {};
    std::wstring result(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                        static_cast<int>(value.size()), result.data(), size);
    return result;
}

std::wstring windowText(HWND window) {
    const int length = GetWindowTextLengthW(window);
    std::wstring result(static_cast<size_t>(length + 1), L'\0');
    GetWindowTextW(window, result.data(), length + 1);
    result.resize(static_cast<size_t>(length));
    return result;
}

std::wstring readRegistryString(const wchar_t* name, const wchar_t* fallback) {
    DWORD bytes = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kRegistryPath, name, RRF_RT_REG_SZ,
                     nullptr, nullptr, &bytes) != ERROR_SUCCESS ||
        bytes < sizeof(wchar_t)) {
        return fallback;
    }
    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    if (RegGetValueW(HKEY_CURRENT_USER, kRegistryPath, name, RRF_RT_REG_SZ,
                     nullptr, value.data(), &bytes) != ERROR_SUCCESS) {
        return fallback;
    }
    if (!value.empty() && value.back() == L'\0') value.pop_back();
    return value;
}

DWORD readRegistryDword(const wchar_t* name, DWORD fallback) {
    DWORD value = fallback;
    DWORD bytes = sizeof(value);
    return RegGetValueW(HKEY_CURRENT_USER, kRegistryPath, name, RRF_RT_REG_DWORD,
                        nullptr, &value, &bytes) == ERROR_SUCCESS ? value : fallback;
}

void writeRegistryString(const wchar_t* name, std::wstring_view value) {
    RegSetKeyValueW(HKEY_CURRENT_USER, kRegistryPath, name, REG_SZ, value.data(),
                    static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
}

void writeRegistryDword(const wchar_t* name, DWORD value) {
    RegSetKeyValueW(HKEY_CURRENT_USER, kRegistryPath, name, REG_DWORD,
                    &value, sizeof(value));
}

AppSettings loadSettings() {
    AppSettings settings;
    settings.outputFolder = utf8(readRegistryString(L"OutputFolder", L""));
    settings.bindAddress = utf8(readRegistryString(L"BindAddress", L"0.0.0.0"));
    const DWORD port = readRegistryDword(L"Port", 20777);
    settings.port = port >= 1 && port <= 65535 ? static_cast<uint16_t>(port) : 20777;
    settings.protocol = tnrp::overrideFromString(
        utf8(readRegistryString(L"Protocol", L"auto")));
    return settings;
}

int protocolIndex(tnrp::Override value) {
    switch (value) {
        case tnrp::Override::F1_24: return 1;
        case tnrp::Override::F1_25: return 2;
        case tnrp::Override::F1_26: return 3;
        default: return 0;
    }
}

tnrp::Override protocolFromIndex(int index) {
    switch (index) {
        case 1: return tnrp::Override::F1_24;
        case 2: return tnrp::Override::F1_25;
        case 3: return tnrp::Override::F1_26;
        default: return tnrp::Override::Auto;
    }
}

struct AppState {
    explicit AppState(AppSettings settings)
        : darkWindowBrush(CreateSolidBrush(kDarkWindowColor)),
          darkControlBrush(CreateSolidBrush(kDarkControlColor)),
          controller(std::move(settings)) {}

    ~AppState() {
        DeleteObject(darkWindowBrush);
        DeleteObject(darkControlBrush);
    }

    HWND window{};
    HWND folder{};
    HWND protocol{};
    HWND address{};
    HWND port{};
    HWND circuit{};
    HWND session{};
    HWND attributionWindow{};
    HFONT font{};
    UINT dpi{96};
    bool initializing{true};
    std::mutex sessionMutex;
    std::wstring pendingCircuit;
    std::wstring pendingSession;
    winrt::Windows::UI::ViewManagement::UISettings uiSettings;
    winrt::event_token colorValuesChangedToken{};
    HBRUSH darkWindowBrush{};
    HBRUSH darkControlBrush{};
    bool darkMode{};
    bool browseButtonHot{};
    bool applyButtonHot{};
    bool attributionButtonHot{};
    // Keep the controller last so its worker threads stop before the callback
    // mutex and pending strings are destroyed.
    MinimalController controller;
};

bool systemUsesDarkMode(AppState& state) {
    using winrt::Windows::UI::ViewManagement::UIColorType;
    const auto foreground = state.uiSettings.GetColorValue(UIColorType::Foreground);
    return 5 * foreground.G + 2 * foreground.R + foreground.B > 8 * 128;
}

void applySystemThemeHints(AppState& state) {
    const bool dark = systemUsesDarkMode(state);
    state.darkMode = dark;

    EnumChildWindows(state.window, [](HWND child, LPARAM value) -> BOOL {
        const bool useDarkMode = value != 0;
        wchar_t className[32]{};
        GetClassNameW(child, className, static_cast<int>(std::size(className)));
        const bool isCombo = _wcsicmp(className, WC_COMBOBOXW) == 0;
        const bool isEdit = _wcsicmp(className, L"EDIT") == 0;
        SetWindowTheme(child,
                       useDarkMode ? ((isCombo || isEdit)
                                          ? L"DarkMode_CFD"
                                          : L"DarkMode_Explorer")
                                   : L"Explorer",
                       nullptr);

        if (isCombo) {
            COMBOBOXINFO info{sizeof(info)};
            if (GetComboBoxInfo(child, &info) && info.hwndList) {
                SetWindowTheme(info.hwndList,
                               useDarkMode ? L"DarkMode_Explorer" : L"Explorer",
                               nullptr);
            }
        }
        return TRUE;
    }, dark ? 1 : 0);

    const BOOL useDarkFrame = dark ? TRUE : FALSE;
    DwmSetWindowAttribute(state.window, DWMWA_USE_IMMERSIVE_DARK_MODE,
                          &useDarkFrame, sizeof(useDarkFrame));
    RedrawWindow(state.window, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
}

AppState* stateFor(HWND window) {
    return reinterpret_cast<AppState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
}

bool& buttonHotState(AppState& state, HWND button) {
    switch (GetDlgCtrlID(button)) {
        case BrowseButton: return state.browseButtonHot;
        case AttributionButton: return state.attributionButtonHot;
        default: return state.applyButtonHot;
    }
}

LRESULT CALLBACK buttonSubclassProc(HWND button, UINT message, WPARAM wParam,
                                    LPARAM lParam, UINT_PTR subclassId,
                                    DWORD_PTR refData) {
    auto& state = *reinterpret_cast<AppState*>(refData);
    switch (message) {
        case WM_MOUSEMOVE: {
            bool& hot = buttonHotState(state, button);
            if (!hot) {
                hot = true;
                TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, button, 0};
                TrackMouseEvent(&tracking);
                InvalidateRect(button, nullptr, FALSE);
            }
            break;
        }
        case WM_MOUSELEAVE:
            buttonHotState(state, button) = false;
            InvalidateRect(button, nullptr, FALSE);
            return 0;
        case WM_GETDLGCODE:
            return GetDlgCtrlID(button) == ApplyButton
                ? DLGC_DEFPUSHBUTTON : DLGC_UNDEFPUSHBUTTON;
        case BM_SETSTYLE:
            // IsDialogMessage changes focused buttons between BS_PUSHBUTTON and
            // BS_DEFPUSHBUTTON. Keep the owner-draw type or Windows takes over
            // painting after the first focus change.
            InvalidateRect(button, nullptr, FALSE);
            return DefSubclassProc(button, message, BS_OWNERDRAW, lParam);
        case WM_NCDESTROY:
            RemoveWindowSubclass(button, buttonSubclassProc, subclassId);
            break;
    }
    return DefSubclassProc(button, message, wParam, lParam);
}

void drawButton(AppState& state, const DRAWITEMSTRUCT& item) {
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const bool focused = (item.itemState & ODS_FOCUS) != 0;
    const bool defaultButton = GetDlgCtrlID(item.hwndItem) == ApplyButton;
    const bool hot = buttonHotState(state, item.hwndItem);

    COLORREF background{};
    COLORREF border{};
    COLORREF textColor{};
    if (state.darkMode) {
        background = disabled ? RGB(40, 40, 40)
                     : pressed ? RGB(35, 35, 35)
                     : hot ? RGB(60, 60, 60)
                     : RGB(48, 48, 48);
        border = (focused || defaultButton) ? RGB(125, 125, 125)
                                            : RGB(88, 88, 88);
        textColor = disabled ? RGB(130, 130, 130) : kDarkTextColor;
    } else {
        background = pressed ? GetSysColor(COLOR_3DSHADOW)
                     : hot ? GetSysColor(COLOR_3DLIGHT)
                     : GetSysColor(COLOR_BTNFACE);
        border = GetSysColor((focused || defaultButton)
            ? COLOR_WINDOWFRAME : COLOR_3DSHADOW);
        textColor = disabled ? GetSysColor(COLOR_GRAYTEXT)
                             : GetSysColor(COLOR_BTNTEXT);
    }

    HBRUSH backgroundBrush = CreateSolidBrush(background);
    FillRect(item.hDC, &item.rcItem, backgroundBrush);
    DeleteObject(backgroundBrush);

    HBRUSH borderBrush = CreateSolidBrush(border);
    FrameRect(item.hDC, &item.rcItem, borderBrush);
    DeleteObject(borderBrush);

    wchar_t text[128]{};
    GetWindowTextW(item.hwndItem, text, static_cast<int>(std::size(text)));
    const HFONT font = reinterpret_cast<HFONT>(
        SendMessageW(item.hwndItem, WM_GETFONT, 0, 0));
    const HGDIOBJ previousFont = font ? SelectObject(item.hDC, font) : nullptr;
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, textColor);
    RECT textRect = item.rcItem;
    if (pressed) OffsetRect(&textRect, 1, 1);
    DrawTextW(item.hDC, text, -1, &textRect,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if (previousFont) SelectObject(item.hDC, previousFont);

}

struct AttributionWindowState {
    explicit AttributionWindowState(AppState* owner)
        : owner(owner),
          darkWindowBrush(CreateSolidBrush(kDarkWindowColor)),
          darkControlBrush(CreateSolidBrush(kDarkControlColor)) {}

    ~AttributionWindowState() {
        DeleteObject(darkWindowBrush);
        DeleteObject(darkControlBrush);
        if (headingFont) DeleteObject(headingFont);
    }

    AppState* owner{};
    HWND window{};
    HWND heading{};
    HWND description{};
    HWND list{};
    HWND licenseHeading{};
    HWND text{};
    HFONT headingFont{};
    HBRUSH darkWindowBrush{};
    HBRUSH darkControlBrush{};
    bool darkMode{};
    winrt::Windows::UI::ViewManagement::UISettings uiSettings;
    winrt::event_token colorValuesChangedToken{};
};

constexpr int kAttributionListId = 300;
constexpr int kAttributionWebsiteId = 301;

std::wstring windowsLineEndings(std::wstring_view input) {
    std::wstring result;
    result.reserve(input.size() + input.size() / 20);
    for (size_t i = 0; i < input.size(); ++i) {
        if (input[i] == L'\n' && (i == 0 || input[i - 1] != L'\r')) result += L'\r';
        result += input[i];
    }
    return result;
}

void showAttributionLicense(AttributionWindowState& state, size_t index) {
    const auto items = minimalAppAttributions();
    if (index >= items.size()) return;
    const Attribution& item = items[index];
    const std::wstring prefix = wide(std::string(item.name) + "  " +
                                     std::string(item.version) + "  -  " +
                                     std::string(item.license) + "  -  " +
                                     std::string(item.copyright) + "  -  ");
    const std::wstring website = wide(item.website);
    const std::wstring metadata = prefix + L"<a href=\"" + website + L"\">" +
                                  website + L"</a>";
    SetWindowTextW(state.licenseHeading, metadata.c_str());

    // Keep the upstream text untouched. The only transformation is CRLF display
    // normalization required by the classic multiline EDIT control.
    const std::wstring text = windowsLineEndings(wide(item.licenseText));
    SetWindowTextW(state.text, text.c_str());
    SendMessageW(state.text, EM_SETSEL, 0, 0);
    SendMessageW(state.text, EM_SCROLLCARET, 0, 0);
}

void drawAttributionRow(AttributionWindowState& state, const DRAWITEMSTRUCT& item) {
    if (item.itemID == static_cast<UINT>(-1)) return;
    const auto attributions = minimalAppAttributions();
    if (item.itemID >= attributions.size()) return;
    const Attribution& attribution = attributions[item.itemID];
    const bool selected = (item.itemState & ODS_SELECTED) != 0;

    const COLORREF page = state.darkMode ? kDarkWindowColor : GetSysColor(COLOR_WINDOW);
    const COLORREF card = state.darkMode
        ? (selected ? RGB(55, 55, 55) : kDarkControlColor)
        : (selected ? GetSysColor(COLOR_3DLIGHT) : GetSysColor(COLOR_WINDOW));
    const COLORREF border = state.darkMode
        ? (selected ? RGB(112, 112, 112) : RGB(67, 67, 67))
        : GetSysColor(selected ? COLOR_HIGHLIGHT : COLOR_3DSHADOW);
    const COLORREF primary = state.darkMode ? kDarkTextColor : GetSysColor(COLOR_WINDOWTEXT);

    HBRUSH pageBrush = CreateSolidBrush(page);
    FillRect(item.hDC, &item.rcItem, pageBrush);
    DeleteObject(pageBrush);

    RECT cardRect = item.rcItem;
    InflateRect(&cardRect, -4, -3);
    HBRUSH cardBrush = CreateSolidBrush(card);
    FillRect(item.hDC, &cardRect, cardBrush);
    DeleteObject(cardBrush);
    HBRUSH borderBrush = CreateSolidBrush(border);
    FrameRect(item.hDC, &cardRect, borderBrush);
    DeleteObject(borderBrush);

    SetBkMode(item.hDC, TRANSPARENT);
    const HFONT font = reinterpret_cast<HFONT>(
        SendMessageW(state.list, WM_GETFONT, 0, 0));
    const HGDIOBJ previousFont = font ? SelectObject(item.hDC, font) : nullptr;

    const std::wstring license = wide(attribution.license);
    const std::wstring version = wide(attribution.version);
    SIZE licenseSize{};
    SIZE versionSize{};
    GetTextExtentPoint32W(item.hDC, license.c_str(), static_cast<int>(license.size()),
                          &licenseSize);
    GetTextExtentPoint32W(item.hDC, version.c_str(), static_cast<int>(version.size()),
                          &versionSize);
    const int badgePad = 8;
    RECT licenseRect = cardRect;
    licenseRect.right -= 9;
    licenseRect.left = licenseRect.right - licenseSize.cx - 2 * badgePad;
    RECT versionRect = licenseRect;
    versionRect.right = licenseRect.left - 7;
    versionRect.left = versionRect.right - versionSize.cx - 2 * badgePad;
    InflateRect(&licenseRect, 0, -8);
    InflateRect(&versionRect, 0, -8);
    HBRUSH badgeBrush = CreateSolidBrush(state.darkMode ? RGB(61, 61, 61)
                                                        : GetSysColor(COLOR_BTNFACE));
    FillRect(item.hDC, &licenseRect, badgeBrush);
    FillRect(item.hDC, &versionRect, badgeBrush);
    DeleteObject(badgeBrush);
    SetTextColor(item.hDC, primary);
    DrawTextW(item.hDC, license.c_str(), -1, &licenseRect,
              DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
    DrawTextW(item.hDC, version.c_str(), -1, &versionRect,
              DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);

    RECT nameRect = cardRect;
    nameRect.left += 12;
    nameRect.right = versionRect.left - 10;
    const std::wstring name = wide(attribution.name);
    DrawTextW(item.hDC, name.c_str(), -1, &nameRect,
              DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    if (previousFont) SelectObject(item.hDC, previousFont);
}

bool systemUsesDarkMode(winrt::Windows::UI::ViewManagement::UISettings& settings) {
    using winrt::Windows::UI::ViewManagement::UIColorType;
    const auto foreground = settings.GetColorValue(UIColorType::Foreground);
    return 5 * foreground.G + 2 * foreground.R + foreground.B > 8 * 128;
}

void applyAttributionTheme(AttributionWindowState& state) {
    state.darkMode = systemUsesDarkMode(state.uiSettings);
    SetWindowTheme(state.list, state.darkMode ? L"DarkMode_Explorer" : L"Explorer", nullptr);
    SetWindowTheme(state.licenseHeading,
                   state.darkMode ? L"DarkMode_Explorer" : L"Explorer", nullptr);
    // The multiline viewer owns a non-client scrollbar. DarkMode_Explorer
    // themes that scrollbar as well as the control; DarkMode_CFD leaves it
    // using the light system scrollbar.
    SetWindowTheme(state.text,
                   state.darkMode ? L"DarkMode_Explorer" : L"Explorer", nullptr);
    const BOOL darkFrame = state.darkMode ? TRUE : FALSE;
    DwmSetWindowAttribute(state.window, DWMWA_USE_IMMERSIVE_DARK_MODE,
                          &darkFrame, sizeof(darkFrame));
    RedrawWindow(state.window, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
}

LRESULT CALLBACK attributionWindowProc(HWND window, UINT message,
                                       WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        const auto create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        auto state = static_cast<AttributionWindowState*>(create->lpCreateParams);
        state->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    auto state = reinterpret_cast<AttributionWindowState*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));

    switch (message) {
        case WM_CREATE: {
            state->heading = CreateWindowExW(0, L"STATIC", L"ATTRIBUTION",
                WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0, window, nullptr,
                GetModuleHandleW(nullptr), nullptr);
            state->description = CreateWindowExW(0, L"STATIC",
                L"Track N Race is built with these open-source components. Thank you to their authors.",
                WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0, window, nullptr,
                GetModuleHandleW(nullptr), nullptr);
            state->list = CreateWindowExW(0, L"LISTBOX", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY |
                    LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT,
                0, 0, 0, 0, window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAttributionListId)),
                GetModuleHandleW(nullptr), nullptr);
            state->licenseHeading = CreateWindowExW(0, WC_LINK, L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 0, 0, window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAttributionWebsiteId)),
                GetModuleHandleW(nullptr), nullptr);
            state->text = CreateWindowExW(
                0, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER |
                    WS_VSCROLL | ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL |
                    ES_READONLY | ES_NOHIDESEL,
                0, 0, 0, 0, window, nullptr, GetModuleHandleW(nullptr), nullptr);
            for (HWND child : {state->heading, state->description, state->list,
                               state->licenseHeading, state->text}) {
                SendMessageW(child, WM_SETFONT,
                             reinterpret_cast<WPARAM>(state->owner->font), TRUE);
            }
            LOGFONTW headingFont{};
            GetObjectW(state->owner->font, sizeof(headingFont), &headingFont);
            headingFont.lfWeight = FW_BOLD;
            state->headingFont = CreateFontIndirectW(&headingFont);
            SendMessageW(state->heading, WM_SETFONT,
                         reinterpret_cast<WPARAM>(state->headingFont), TRUE);
            SendMessageW(state->text, EM_SETLIMITTEXT, 0, 0);
            for (const Attribution& item : minimalAppAttributions()) {
                const std::wstring name = wide(item.name);
                SendMessageW(state->list, LB_ADDSTRING, 0,
                             reinterpret_cast<LPARAM>(name.c_str()));
            }
            SendMessageW(state->list, LB_SETCURSEL, 0, 0);
            showAttributionLicense(*state, 0);
            applyAttributionTheme(*state);
            state->colorValuesChangedToken = state->uiSettings.ColorValuesChanged(
                [state](auto const&, auto const&) {
                    PostMessageW(state->window, kSystemThemeChanged, 0, 0);
                });
            return 0;
        }
        case WM_SIZE:
            if (state && state->text) {
                RECT client{};
                GetClientRect(window, &client);
                const UINT dpi = GetDpiForWindow(window);
                const int margin = MulDiv(18, static_cast<int>(dpi), 96);
                const int width = std::max(0, static_cast<int>(client.right) - 2 * margin);
                const int headingHeight = MulDiv(20, static_cast<int>(dpi), 96);
                const int descriptionHeight = MulDiv(24, static_cast<int>(dpi), 96);
                const int listHeight = MulDiv(242, static_cast<int>(dpi), 96);
                const int gap = MulDiv(8, static_cast<int>(dpi), 96);
                int y = margin;
                SetWindowPos(state->heading, nullptr, margin, y, width, headingHeight,
                             SWP_NOZORDER | SWP_NOACTIVATE);
                y += headingHeight;
                SetWindowPos(state->description, nullptr, margin, y, width,
                             descriptionHeight, SWP_NOZORDER | SWP_NOACTIVATE);
                y += descriptionHeight + gap;
                SetWindowPos(state->list, nullptr, margin, y, width, listHeight,
                             SWP_NOZORDER | SWP_NOACTIVATE);
                y += listHeight + gap;
                SetWindowPos(state->licenseHeading, nullptr, margin, y, width,
                             headingHeight, SWP_NOZORDER | SWP_NOACTIVATE);
                y += headingHeight + gap;
                SetWindowPos(state->text, nullptr, margin, y, width,
                             std::max(0, static_cast<int>(client.bottom) - margin - y),
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
            return 0;
        case WM_MEASUREITEM:
            if (state) {
                auto measure = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
                if (measure->CtlID == kAttributionListId) {
                    measure->itemHeight = MulDiv(46,
                        static_cast<int>(GetDpiForWindow(window)), 96);
                    return TRUE;
                }
            }
            break;
        case WM_DRAWITEM:
            if (state) {
                const auto item = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
                if (item->CtlID == kAttributionListId) {
                    drawAttributionRow(*state, *item);
                    return TRUE;
                }
            }
            break;
        case WM_COMMAND:
            if (state && LOWORD(wParam) == kAttributionListId &&
                HIWORD(wParam) == LBN_SELCHANGE) {
                const LRESULT selected = SendMessageW(state->list, LB_GETCURSEL, 0, 0);
                if (selected != LB_ERR) {
                    showAttributionLicense(*state, static_cast<size_t>(selected));
                }
                return 0;
            }
            break;
        case WM_NOTIFY:
            if (state) {
                const auto header = reinterpret_cast<NMHDR*>(lParam);
                if (header->idFrom == kAttributionWebsiteId &&
                    (header->code == NM_CLICK || header->code == NM_RETURN)) {
                    const auto link = reinterpret_cast<NMLINK*>(lParam);
                    ShellExecuteW(window, L"open", link->item.szUrl,
                                  nullptr, nullptr, SW_SHOWNORMAL);
                    return 0;
                }
            }
            break;
        case WM_ERASEBKGND:
            if (state && state->darkMode) {
                RECT client{};
                GetClientRect(window, &client);
                FillRect(reinterpret_cast<HDC>(wParam), &client, state->darkWindowBrush);
                return 1;
            }
            break;
        case WM_CTLCOLORSTATIC:
            if (state && state->darkMode) {
                const HDC dc = reinterpret_cast<HDC>(wParam);
                const HWND control = reinterpret_cast<HWND>(lParam);
                const bool isLicenseText = control == state->text;
                SetTextColor(dc, isLicenseText ? RGB(205, 205, 205) : kDarkTextColor);
                SetBkColor(dc, isLicenseText ? kDarkControlColor : kDarkWindowColor);
                return reinterpret_cast<LRESULT>(isLicenseText
                    ? state->darkControlBrush : state->darkWindowBrush);
            }
            break;
        case WM_CTLCOLORLISTBOX:
            if (state && state->darkMode) {
                const HDC dc = reinterpret_cast<HDC>(wParam);
                SetTextColor(dc, kDarkTextColor);
                SetBkColor(dc, kDarkWindowColor);
                return reinterpret_cast<LRESULT>(state->darkWindowBrush);
            }
            break;
        case kSystemThemeChanged:
            if (state) applyAttributionTheme(*state);
            return 0;
        case WM_DESTROY:
            if (state) {
                state->uiSettings.ColorValuesChanged(state->colorValuesChangedToken);
                if (state->owner) {
                    state->owner->attributionWindow = nullptr;
                    EnableWindow(state->owner->window, TRUE);
                    SetForegroundWindow(state->owner->window);
                }
            }
            return 0;
        case WM_NCDESTROY:
            if (state) {
                SetWindowLongPtrW(window, GWLP_USERDATA, 0);
                delete state;
            }
            return DefWindowProcW(window, message, wParam, lParam);
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

void showAttributions(AppState& state) {
    if (state.attributionWindow) {
        ShowWindow(state.attributionWindow, SW_RESTORE);
        SetForegroundWindow(state.attributionWindow);
        return;
    }

    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.lpfnWndProc = attributionWindowProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.hIcon = LoadIconW(windowClass.hInstance, MAKEINTRESOURCEW(1));
    windowClass.hIconSm = windowClass.hIcon;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    windowClass.lpszClassName = kAttributionWindowClass;
    RegisterClassExW(&windowClass);

    auto attributionState = std::make_unique<AttributionWindowState>(&state);
    const UINT dpi = GetDpiForWindow(state.window);
    HWND window = CreateWindowExW(
        0, kAttributionWindowClass, L"Attributions - Track N Race Minimal Recorder",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT,
        MulDiv(805, static_cast<int>(dpi), 96),
        MulDiv(584, static_cast<int>(dpi), 96),
        state.window, nullptr, GetModuleHandleW(nullptr), attributionState.get());
    if (!window) return;
    state.attributionWindow = window;
    attributionState.release();
    EnableWindow(state.window, FALSE);
    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);
}

int scaled(AppState& state, int value) {
    return MulDiv(value, static_cast<int>(state.dpi), 96);
}

int nativeControlHeight(AppState& state) {
    HDC dc = GetDC(state.window);
    const HGDIOBJ previous = SelectObject(dc, state.font);
    TEXTMETRICW metrics{};
    GetTextMetricsW(dc, &metrics);
    SelectObject(dc, previous);
    ReleaseDC(state.window, dc);

    const int edge = GetSystemMetricsForDpi(SM_CYEDGE, state.dpi);
    const int border = GetSystemMetricsForDpi(SM_CYBORDER, state.dpi);
    return metrics.tmHeight + 2 * edge + 2 * border;
}

HWND addControl(AppState& state, const wchar_t* className, const wchar_t* text,
                DWORD style, int id, DWORD extendedStyle = 0) {
    HWND control = CreateWindowExW(extendedStyle, className, text,
                                   WS_CHILD | WS_VISIBLE | style,
                                   0, 0, 0, 0, state.window,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                   GetModuleHandleW(nullptr), nullptr);
    if (state.font) SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(state.font), TRUE);
    return control;
}

void applyFont(AppState& state) {
    NONCLIENTMETRICSW metrics{sizeof(metrics)};
    using GetMetricsForDpi = BOOL(WINAPI*)(UINT, UINT, PVOID, UINT, UINT);
    const auto getForDpi = reinterpret_cast<GetMetricsForDpi>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "SystemParametersInfoForDpi"));
    const BOOL ok = getForDpi
        ? getForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, state.dpi)
        : SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0);
    HFONT replacement = ok ? CreateFontIndirectW(&metrics.lfMessageFont)
                           : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    if (state.font && state.font != GetStockObject(DEFAULT_GUI_FONT)) DeleteObject(state.font);
    state.font = replacement;
    EnumChildWindows(state.window, [](HWND child, LPARAM value) -> BOOL {
        SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(value), TRUE);
        return TRUE;
    }, reinterpret_cast<LPARAM>(state.font));
}

void layout(AppState& state) {
    RECT client{};
    GetClientRect(state.window, &client);
    const int margin = scaled(state, 20);
    const int labelWidth = scaled(state, 125);
    const int gap = scaled(state, 10);
    const int buttonWidth = scaled(state, 110);
    const int rowHeight = nativeControlHeight(state);
    const int rowGap = scaled(state, 8);
    const int valueX = margin + labelWidth;
    const int right = std::max(valueX + scaled(state, 180),
                               static_cast<int>(client.right) - margin);
    const int fieldWidth = right - valueX;

    const int rows[] = {margin, margin + rowHeight + rowGap,
                        margin + 2 * (rowHeight + rowGap),
                        margin + 3 * (rowHeight + rowGap),
                        margin + 5 * (rowHeight + rowGap),
                        margin + 6 * (rowHeight + rowGap)};

    auto move = [](HWND control, int x, int y, int width, int height) {
        SetWindowPos(control, nullptr, x, y, width, height,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    };
    const int labels[] = {200, 201, 202, 203, 204, 205};
    for (size_t i = 0; i < std::size(labels); ++i) {
        move(GetDlgItem(state.window, labels[i]), margin, rows[i],
             labelWidth - gap, rowHeight);
    }
    move(state.folder, valueX, rows[0], fieldWidth - buttonWidth - gap, rowHeight);
    move(GetDlgItem(state.window, BrowseButton), right - buttonWidth, rows[0],
         buttonWidth, rowHeight);
    move(state.protocol, valueX, rows[1], fieldWidth, scaled(state, 220));
    move(state.address, valueX, rows[2], fieldWidth, rowHeight);
    move(state.port, valueX, rows[3], fieldWidth - buttonWidth - gap, rowHeight);
    move(GetDlgItem(state.window, ApplyButton), right - buttonWidth, rows[3],
         buttonWidth, rowHeight);
    move(state.circuit, valueX, rows[4], fieldWidth, rowHeight);
    move(state.session, valueX, rows[5], fieldWidth - buttonWidth - gap, rowHeight);
    move(GetDlgItem(state.window, AttributionButton), right - buttonWidth, rows[5],
         buttonWidth, rowHeight);
}

void showError(HWND owner, std::string_view error) {
    MessageBoxW(owner, wide(error).c_str(), L"Track N Race Minimal Recorder",
                MB_OK | MB_ICONERROR);
}

void chooseFolder(AppState& state) {
    IFileOpenDialog* dialog = nullptr;
    HRESULT result = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&dialog));
    if (FAILED(result)) {
        MessageBoxW(state.window, L"The Windows folder picker could not be opened.",
                    L"Track N Race Minimal Recorder", MB_OK | MB_ICONERROR);
        return;
    }

    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    result = dialog->Show(state.window);
    if (SUCCEEDED(result)) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                std::string error;
                if (state.controller.setOutputFolder(utf8(path), error)) {
                    SetWindowTextW(state.folder, path);
                    writeRegistryString(L"OutputFolder", path);
                } else {
                    showError(state.window, error);
                }
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dialog->Release();
}

void applyNetwork(AppState& state) {
    const std::wstring portText = windowText(state.port);
    unsigned long parsedPort = 0;
    wchar_t* end = nullptr;
    parsedPort = wcstoul(portText.c_str(), &end, 10);
    if (portText.empty() || !end || *end != L'\0' || parsedPort < 1 || parsedPort > 65535) {
        MessageBoxW(state.window, L"The UDP port must be between 1 and 65535.",
                    L"Track N Race Minimal Recorder", MB_OK | MB_ICONERROR);
        return;
    }

    const std::wstring address = windowText(state.address);
    std::string error;
    if (!state.controller.applyNetwork(utf8(address), static_cast<uint16_t>(parsedPort), error)) {
        showError(state.window, error);
        const AppSettings& settings = state.controller.settings();
        SetWindowTextW(state.address, wide(settings.bindAddress).c_str());
        SetWindowTextW(state.port, std::to_wstring(settings.port).c_str());
        return;
    }
    writeRegistryString(L"BindAddress", address);
    writeRegistryDword(L"Port", static_cast<DWORD>(parsedPort));
}

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        const auto create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        const auto state = static_cast<AppState*>(create->lpCreateParams);
        state->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    AppState* state = stateFor(window);

    switch (message) {
        case WM_CREATE: {
            state->dpi = GetDpiForWindow(window);
            applyFont(*state);
            addControl(*state, L"STATIC", L"Recording folder", SS_CENTERIMAGE, 200);
            addControl(*state, L"STATIC", L"Protocol override", SS_CENTERIMAGE, 201);
            addControl(*state, L"STATIC", L"IPv4 bind address", SS_CENTERIMAGE, 202);
            addControl(*state, L"STATIC", L"UDP port", SS_CENTERIMAGE, 203);
            addControl(*state, L"STATIC", L"Circuit name", SS_CENTERIMAGE, 204);
            addControl(*state, L"STATIC", L"Session", SS_CENTERIMAGE, 205);

            state->folder = addControl(*state, L"EDIT", L"",
                WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL | ES_READONLY, FolderEdit);
            HWND browseButton = addControl(*state, L"BUTTON", L"Browse...",
                                           WS_TABSTOP | BS_OWNERDRAW, BrowseButton);
            SetWindowSubclass(browseButton, buttonSubclassProc, BrowseButton,
                              reinterpret_cast<DWORD_PTR>(state));
            state->protocol = addControl(*state, WC_COMBOBOXW, L"",
                WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, ProtocolCombo);
            for (const wchar_t* item : {L"Auto", L"F1 24", L"F1 25", L"F1 26"}) {
                SendMessageW(state->protocol, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item));
            }
            state->address = addControl(*state, L"EDIT", L"",
                WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL, AddressEdit);
            state->port = addControl(*state, L"EDIT", L"",
                WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL | ES_NUMBER, PortEdit);
            HWND applyButton = addControl(*state, L"BUTTON", L"Apply network",
                                          WS_TABSTOP | BS_OWNERDRAW, ApplyButton);
            SetWindowSubclass(applyButton, buttonSubclassProc, ApplyButton,
                              reinterpret_cast<DWORD_PTR>(state));
            state->circuit = addControl(*state, L"STATIC", L"Unavailable", SS_CENTERIMAGE, CircuitValue);
            state->session = addControl(*state, L"STATIC", L"Unavailable", SS_CENTERIMAGE, SessionValue);
            HWND attributionButton = addControl(*state, L"BUTTON", L"Attributions...",
                                                WS_TABSTOP | BS_OWNERDRAW,
                                                AttributionButton);
            SetWindowSubclass(attributionButton, buttonSubclassProc, AttributionButton,
                              reinterpret_cast<DWORD_PTR>(state));

            applySystemThemeHints(*state);
            state->colorValuesChangedToken = state->uiSettings.ColorValuesChanged(
                [state](auto const&, auto const&) {
                    PostMessageW(state->window, kSystemThemeChanged, 0, 0);
                });

            const AppSettings& settings = state->controller.settings();
            SetWindowTextW(state->folder, wide(settings.outputFolder).c_str());
            SetWindowTextW(state->address, wide(settings.bindAddress).c_str());
            SetWindowTextW(state->port, std::to_wstring(settings.port).c_str());
            SendMessageW(state->protocol, CB_SETCURSEL, protocolIndex(settings.protocol), 0);
            state->initializing = false;
            layout(*state);

            state->controller.setSessionCallback([state](std::string circuit, std::string session) {
                {
                    std::lock_guard<std::mutex> lock(state->sessionMutex);
                    state->pendingCircuit = wide(circuit);
                    state->pendingSession = wide(session);
                }
                PostMessageW(state->window, kSessionUpdated, 0, 0);
            });
            std::string error;
            if (!state->controller.start(error)) showError(window, error);
            return 0;
        }
        case WM_SIZE:
            if (state) layout(*state);
            return 0;
        case WM_DPICHANGED:
            if (state) {
                state->dpi = HIWORD(wParam);
                const auto rect = reinterpret_cast<RECT*>(lParam);
                SetWindowPos(window, nullptr, rect->left, rect->top,
                             rect->right - rect->left, rect->bottom - rect->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
                applyFont(*state);
                layout(*state);
            }
            return 0;
        case WM_ERASEBKGND:
            if (state && state->darkMode) {
                RECT client{};
                GetClientRect(window, &client);
                FillRect(reinterpret_cast<HDC>(wParam), &client, state->darkWindowBrush);
                return 1;
            }
            break;
        case WM_CTLCOLORSTATIC:
            if (state && state->darkMode) {
                const HDC dc = reinterpret_cast<HDC>(wParam);
                const HWND control = reinterpret_cast<HWND>(lParam);
                wchar_t className[16]{};
                GetClassNameW(control, className, static_cast<int>(std::size(className)));
                const bool isReadOnlyEdit = _wcsicmp(className, L"EDIT") == 0;
                const COLORREF background = isReadOnlyEdit
                    ? kDarkControlColor : kDarkWindowColor;
                SetTextColor(dc, kDarkTextColor);
                SetBkColor(dc, background);
                return reinterpret_cast<LRESULT>(isReadOnlyEdit
                    ? state->darkControlBrush : state->darkWindowBrush);
            }
            break;
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
            if (state && state->darkMode) {
                const HDC dc = reinterpret_cast<HDC>(wParam);
                SetTextColor(dc, kDarkTextColor);
                SetBkColor(dc, kDarkControlColor);
                return reinterpret_cast<LRESULT>(state->darkControlBrush);
            }
            break;
        case WM_DRAWITEM:
            if (state) {
                const auto item = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
                if (item->CtlType == ODT_BUTTON &&
                    (item->CtlID == BrowseButton || item->CtlID == ApplyButton ||
                     item->CtlID == AttributionButton)) {
                    drawButton(*state, *item);
                    return TRUE;
                }
            }
            break;
        case WM_COMMAND:
            if (!state) break;
            switch (LOWORD(wParam)) {
                case BrowseButton:
                    if (HIWORD(wParam) == BN_CLICKED) chooseFolder(*state);
                    return 0;
                case ApplyButton:
                    if (HIWORD(wParam) == BN_CLICKED) applyNetwork(*state);
                    return 0;
                case AttributionButton:
                    if (HIWORD(wParam) == BN_CLICKED) showAttributions(*state);
                    return 0;
                case ProtocolCombo:
                    if (HIWORD(wParam) == CBN_SELCHANGE && !state->initializing) {
                        const int index = static_cast<int>(
                            SendMessageW(state->protocol, CB_GETCURSEL, 0, 0));
                        const tnrp::Override protocol = protocolFromIndex(index);
                        state->controller.setProtocol(protocol);
                        writeRegistryString(L"Protocol", wide(tnrp::toString(protocol)));
                    }
                    return 0;
            }
            break;
        case kSessionUpdated:
            if (state) {
                std::lock_guard<std::mutex> lock(state->sessionMutex);
                SetWindowTextW(state->circuit, state->pendingCircuit.c_str());
                SetWindowTextW(state->session, state->pendingSession.c_str());
            }
            return 0;
        case kSystemThemeChanged:
            if (state) applySystemThemeHints(*state);
            return 0;
        case WM_DESTROY:
            if (state) {
                state->uiSettings.ColorValuesChanged(state->colorValuesChangedToken);
                state->controller.setSessionCallback({});
            }
            PostQuitMessage(0);
            return 0;
        case WM_NCDESTROY:
            if (state) {
                SetWindowLongPtrW(window, GWLP_USERDATA, 0);
                if (state->font && state->font != GetStockObject(DEFAULT_GUI_FONT)) {
                    DeleteObject(state->font);
                }
                delete state;
            }
            return DefWindowProcW(window, message, wParam, lParam);
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES | ICC_LINK_CLASS};
    InitCommonControlsEx(&controls);

    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = windowProc;
    windowClass.hInstance = instance;
    windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    windowClass.hIconSm = windowClass.hIcon;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    // A form made from STATIC/EDIT/BUTTON controls uses the system dialog-face
    // brush. COLOR_WINDOW is for document surfaces and makes native STATIC
    // controls appear as separate gray boxes against a white client area.
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    windowClass.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&windowClass)) return 1;

    auto state = std::make_unique<AppState>(loadSettings());
    const UINT dpi = GetDpiForSystem();
    const int width = MulDiv(646, static_cast<int>(dpi), 96);
    const int height = MulDiv(268, static_cast<int>(dpi), 96);
    HWND window = CreateWindowExW(0, kWindowClass, L"Track N Race Minimal Recorder",
                                  WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                  CW_USEDEFAULT, CW_USEDEFAULT,
                                  width, height, nullptr, nullptr, instance, state.get());
    if (!window) return 1;
    state.release();

    ShowWindow(window, showCommand);
    UpdateWindow(window);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    if (SUCCEEDED(comResult)) CoUninitialize();
    return static_cast<int>(message.wParam);
}
