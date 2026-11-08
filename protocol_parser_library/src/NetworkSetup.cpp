#include "tnrp/NetworkSetup.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <iterator>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <iphlpapi.h>
#  include <windows.h>
#  include <objbase.h>
#  include <oleauto.h>
#  include <netfw.h>
#  include <shellapi.h>
#else
#  include <arpa/inet.h>
#  include <ifaddrs.h>
#  include <net/if.h>
#  include <netinet/in.h>
#  include <sys/stat.h>
#endif

namespace tnrp {

namespace {

std::string lowerAscii(std::string_view value) {
    std::string out(value);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool startsWith(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

// Interface-name prefixes used by virtual adapters on Linux, macOS and BSD.
constexpr std::string_view kVirtualNamePrefixes[] = {
    "veth", "docker", "br-", "virbr", "vmnet", "vboxnet", "tun", "tap", "utun", "wg", "zt",
    "tailscale", "awdl", "llw", "anpi", "bridge", "gif", "stf", "lxc", "lxd", "cni", "flannel",
    "podman", "vnic", "ham", "ipsec", "ppp",
};

// Words that mark an adapter as virtual in a Windows adapter name or description.
constexpr std::string_view kVirtualWords[] = {
    "virtual", "vethernet", "hyper-v", "wsl", "vmware", "virtualbox", "vpn", "wireguard", "wintun",
    "tap-windows", "tap adapter", "openvpn", "tailscale", "zerotier", "npcap", "bluetooth", "hamachi",
    "radmin", "teredo", "isatap", "6to4", "docker", "nordlynx", "anyconnect", "fortinet", "pangp",
    "kernel debug", "wan miniport",
};

// Windows IANA interface types.
constexpr int kIfTypeEthernet = 6;
constexpr int kIfTypePpp = 23;
constexpr int kIfTypeLoopback = 24;
constexpr int kIfTypePropVirtual = 53;
constexpr int kIfTypeWifi = 71;
constexpr int kIfTypeTunnel = 131;

bool isLinkLocal(const std::string& address) { return startsWith(address, "169.254."); }
bool isLoopbackAddress(const std::string& address) { return startsWith(address, "127."); }

// Sorts by kind, keeps one entry per address, and makes the loopback entry the
// single 127.0.0.1 row at the end.
std::vector<NetworkInterfaceInfo> finishList(std::vector<std::pair<NetworkInterfaceKind, NetworkInterfaceInfo>> found) {
    std::string loopbackName = "Loopback";
    std::vector<std::pair<NetworkInterfaceKind, NetworkInterfaceInfo>> kept;
    for (auto& entry : found) {
        if (isLinkLocal(entry.second.address)) continue;
        if (entry.first == NetworkInterfaceKind::Loopback || isLoopbackAddress(entry.second.address)) {
            if (!entry.second.name.empty()) loopbackName = entry.second.name;
            continue;
        }
        const bool duplicate = std::any_of(kept.begin(), kept.end(), [&](const auto& existing) {
            return existing.second.address == entry.second.address;
        });
        if (!duplicate) kept.push_back(std::move(entry));
    }
    std::stable_sort(kept.begin(), kept.end(), [](const auto& a, const auto& b) {
        if (a.first != b.first) return static_cast<int>(a.first) < static_cast<int>(b.first);
        return a.second.name < b.second.name;
    });
    std::vector<NetworkInterfaceInfo> out;
    out.reserve(kept.size() + 1);
    for (auto& entry : kept) out.push_back(std::move(entry.second));
    out.push_back({loopbackName, networkInterfaceKindName(NetworkInterfaceKind::Loopback), "127.0.0.1"});
    return out;
}

#ifdef _WIN32

std::string toUtf8(const wchar_t* value) {
    if (!value || !*value) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return {};
    std::string out(static_cast<size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, -1, out.data(), size, nullptr, nullptr);
    return out;
}

std::wstring toWide(const std::string& value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    if (size <= 1) return {};
    std::wstring out(static_cast<size_t>(size - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, out.data(), size);
    return out;
}

// GUIDs are spelled out so the file builds with both MSVC and MinGW headers.
const GUID kClsidNetFwPolicy2 = {0xE2B3C97F, 0x6AE1, 0x41AC, {0x81, 0x7A, 0xF6, 0xF9, 0x21, 0x66, 0xD7, 0xDD}};
const GUID kIidNetFwPolicy2 = {0x98325047, 0xC671, 0x4174, {0x8D, 0x81, 0xDE, 0xFC, 0xD3, 0xF0, 0x31, 0x86}};
const GUID kIidNetFwRule = {0xAF230D27, 0xBABA, 0x4E42, {0xAC, 0xED, 0xF5, 0x24, 0xF2, 0x2C, 0xFC, 0xE2}};
const GUID kIidEnumVariant = {0x00020404, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

struct ComScope {
    HRESULT hr;
    explicit ComScope(DWORD model) : hr(CoInitializeEx(nullptr, model)) {}
    ~ComScope() { if (SUCCEEDED(hr)) CoUninitialize(); }
    bool usable() const { return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE; }
};

template <class T>
struct ComRef {
    T* p = nullptr;
    ComRef() = default;
    ComRef(const ComRef&) = delete;
    ComRef& operator=(const ComRef&) = delete;
    ~ComRef() { if (p) p->Release(); }
    void** put() { if (p) { p->Release(); p = nullptr; } return reinterpret_cast<void**>(&p); }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

struct BstrRef {
    BSTR s = nullptr;
    ~BstrRef() { if (s) SysFreeString(s); }
};

std::wstring normalizedPath(const std::wstring& path) {
    if (path.empty()) return {};
    wchar_t expanded[MAX_PATH * 2];
    const DWORD n = ExpandEnvironmentStringsW(path.c_str(), expanded, static_cast<DWORD>(std::size(expanded)));
    std::wstring value = (n > 0 && n <= std::size(expanded)) ? std::wstring(expanded) : path;
    wchar_t full[MAX_PATH * 2];
    const DWORD m = GetFullPathNameW(value.c_str(), static_cast<DWORD>(std::size(full)), full, nullptr);
    if (m > 0 && m < std::size(full)) value = full;
    std::transform(value.begin(), value.end(), value.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return value;
}

bool portListMatches(const wchar_t* list, uint16_t port) {
    if (!list || !*list) return true;
    const std::string text = toUtf8(list);
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find(',', start);
        if (end == std::string::npos) end = text.size();
        std::string token = text.substr(start, end - start);
        token.erase(std::remove_if(token.begin(), token.end(), [](unsigned char c) { return std::isspace(c); }), token.end());
        if (token == "*") return true;
        const size_t dash = token.find('-');
        const auto allDigits = [](const std::string& s) {
            return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c); });
        };
        if (dash != std::string::npos) {
            const std::string lo = token.substr(0, dash), hi = token.substr(dash + 1);
            if (allDigits(lo) && allDigits(hi) && std::atoi(lo.c_str()) <= port && port <= std::atoi(hi.c_str())) return true;
        } else if (allDigits(token) && std::atoi(token.c_str()) == port) {
            return true;
        }
        start = end + 1;
    }
    return false;
}

std::wstring base64Utf16(const std::wstring& text) {
    static const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string bytes;
    bytes.reserve(text.size() * 2);
    for (wchar_t c : text) {
        bytes.push_back(static_cast<char>(c & 0xFF));
        bytes.push_back(static_cast<char>((c >> 8) & 0xFF));
    }
    std::wstring out;
    size_t i = 0;
    for (; i + 2 < bytes.size(); i += 3) {
        const unsigned v = (static_cast<unsigned char>(bytes[i]) << 16) |
                           (static_cast<unsigned char>(bytes[i + 1]) << 8) | static_cast<unsigned char>(bytes[i + 2]);
        out += alphabet[(v >> 18) & 63]; out += alphabet[(v >> 12) & 63];
        out += alphabet[(v >> 6) & 63];  out += alphabet[v & 63];
    }
    if (i < bytes.size()) {
        unsigned v = static_cast<unsigned char>(bytes[i]) << 16;
        if (i + 1 < bytes.size()) v |= static_cast<unsigned char>(bytes[i + 1]) << 8;
        out += alphabet[(v >> 18) & 63]; out += alphabet[(v >> 12) & 63];
        out += (i + 1 < bytes.size()) ? alphabet[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

#endif

} // namespace

const char* networkInterfaceKindName(NetworkInterfaceKind kind) {
    switch (kind) {
        case NetworkInterfaceKind::Wifi: return "wifi";
        case NetworkInterfaceKind::Ethernet: return "ethernet";
        case NetworkInterfaceKind::Virtual: return "virtual";
        case NetworkInterfaceKind::Loopback: return "loopback";
        case NetworkInterfaceKind::Other: break;
    }
    return "other";
}

NetworkInterfaceKind classifyNetworkInterface(std::string_view name, std::string_view description,
                                              int ifType, bool wireless) {
    const std::string n = lowerAscii(name), d = lowerAscii(description);
    if (ifType == kIfTypeLoopback || n == "lo" || n == "lo0" || d.find("loopback pseudo") != std::string::npos)
        return NetworkInterfaceKind::Loopback;
    for (const auto word : kVirtualWords) {
        if (n.find(word) != std::string::npos || d.find(word) != std::string::npos) return NetworkInterfaceKind::Virtual;
    }
    for (const auto prefix : kVirtualNamePrefixes) {
        if (startsWith(n, prefix)) return NetworkInterfaceKind::Virtual;
    }
    if (ifType == kIfTypeTunnel || ifType == kIfTypePpp || ifType == kIfTypePropVirtual) return NetworkInterfaceKind::Virtual;
    if (wireless || ifType == kIfTypeWifi || startsWith(n, "wl")) return NetworkInterfaceKind::Wifi;
    if (ifType == kIfTypeEthernet || startsWith(n, "en") || startsWith(n, "eth")) return NetworkInterfaceKind::Ethernet;
    return NetworkInterfaceKind::Other;
}

std::vector<NetworkInterfaceInfo> listNetworkInterfaces() {
    std::vector<std::pair<NetworkInterfaceKind, NetworkInterfaceInfo>> found;
#ifdef _WIN32
    ULONG size = 16 * 1024;
    std::vector<unsigned char> buffer;
    ULONG result = ERROR_BUFFER_OVERFLOW;
    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    for (int attempt = 0; attempt < 4 && result == ERROR_BUFFER_OVERFLOW; ++attempt) {
        buffer.resize(size);
        result = GetAdaptersAddresses(AF_INET, flags, nullptr,
                                      reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    }
    if (result == NO_ERROR) {
        for (auto* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); adapter; adapter = adapter->Next) {
            if (adapter->OperStatus != IfOperStatusUp) continue;
            const std::string name = toUtf8(adapter->FriendlyName);
            const std::string description = toUtf8(adapter->Description);
            const int ifType = static_cast<int>(adapter->IfType);
            const NetworkInterfaceKind kind = classifyNetworkInterface(name, description, ifType, ifType == kIfTypeWifi);
            for (auto* unicast = adapter->FirstUnicastAddress; unicast; unicast = unicast->Next) {
                const sockaddr* address = unicast->Address.lpSockaddr;
                if (!address || address->sa_family != AF_INET) continue;
                char text[INET_ADDRSTRLEN] = {};
                if (!inet_ntop(AF_INET, &reinterpret_cast<const sockaddr_in*>(address)->sin_addr, text, sizeof(text))) continue;
                found.push_back({kind, {name, networkInterfaceKindName(kind), text}});
            }
        }
    }
#else
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) == 0) {
        for (ifaddrs* entry = list; entry; entry = entry->ifa_next) {
            if (!entry->ifa_addr || entry->ifa_addr->sa_family != AF_INET || !entry->ifa_name) continue;
            if (!(entry->ifa_flags & IFF_UP)) continue;
            const std::string name = entry->ifa_name;
            bool wireless = false;
#  if defined(__linux__)
            struct stat info {};
            wireless = stat(("/sys/class/net/" + name + "/wireless").c_str(), &info) == 0;
#  endif
            NetworkInterfaceKind kind = (entry->ifa_flags & IFF_LOOPBACK)
                ? NetworkInterfaceKind::Loopback
                : classifyNetworkInterface(name, {}, -1, wireless);
#  if defined(__APPLE__)
            // macOS names Wi-Fi and wired adapters alike (en0, en1), so don't guess.
            if (kind == NetworkInterfaceKind::Ethernet) kind = NetworkInterfaceKind::Other;
#  endif
            char text[INET_ADDRSTRLEN] = {};
            if (!inet_ntop(AF_INET, &reinterpret_cast<const sockaddr_in*>(entry->ifa_addr)->sin_addr, text, sizeof(text))) continue;
            found.push_back({kind, {name, networkInterfaceKindName(kind), text}});
        }
        freeifaddrs(list);
    }
#endif
    return finishList(std::move(found));
}

std::string networkInterfacesJson() {
    std::string out;
    (void)glz::write_json(listNetworkInterfaces(), out);
    return out;
}

FirewallStatus firewallStatus(uint16_t port, const std::string& programPath) {
    FirewallStatus status;
#ifdef _WIN32
    status.supported = true;
    ComScope com(COINIT_MULTITHREADED);
    if (!com.usable()) {
        status.error = "COM could not be initialised.";
        return status;
    }
    ComRef<INetFwPolicy2> policy;
    if (FAILED(CoCreateInstance(kClsidNetFwPolicy2, nullptr, CLSCTX_INPROC_SERVER, kIidNetFwPolicy2, policy.put())) || !policy) {
        status.error = "Windows Firewall could not be queried.";
        return status;
    }
    long profiles = 0;
    if (FAILED(policy->get_CurrentProfileTypes(&profiles))) {
        status.error = "Windows Firewall could not report the active network profile.";
        return status;
    }
    status.domainNetwork = (profiles & NET_FW_PROFILE2_DOMAIN) != 0;
    status.privateNetwork = (profiles & NET_FW_PROFILE2_PRIVATE) != 0;
    status.publicNetwork = (profiles & NET_FW_PROFILE2_PUBLIC) != 0;

    bool defaultAllowsAll = true;
    for (const long profile : {static_cast<long>(NET_FW_PROFILE2_DOMAIN), static_cast<long>(NET_FW_PROFILE2_PRIVATE),
                               static_cast<long>(NET_FW_PROFILE2_PUBLIC)}) {
        if (!(profiles & profile)) continue;
        VARIANT_BOOL on = VARIANT_FALSE;
        if (FAILED(policy->get_FirewallEnabled(static_cast<NET_FW_PROFILE_TYPE2>(profile), &on)) || on != VARIANT_TRUE) continue;
        status.enabled = true;
        NET_FW_ACTION action = NET_FW_ACTION_BLOCK;
        if (FAILED(policy->get_DefaultInboundAction(static_cast<NET_FW_PROFILE_TYPE2>(profile), &action)) ||
            action != NET_FW_ACTION_ALLOW)
            defaultAllowsAll = false;
    }
    if (!status.enabled) {
        status.allowed = "yes";
        return status;
    }

    ComRef<INetFwRules> rules;
    ComRef<IUnknown> enumUnknown;
    ComRef<IEnumVARIANT> enumerator;
    if (FAILED(policy->get_Rules(reinterpret_cast<INetFwRules**>(rules.put()))) || !rules ||
        FAILED(rules->get__NewEnum(reinterpret_cast<IUnknown**>(enumUnknown.put()))) || !enumUnknown ||
        FAILED(enumUnknown->QueryInterface(kIidEnumVariant, enumerator.put())) || !enumerator) {
        status.error = "Windows Firewall rules could not be read.";
        return status;
    }

    const std::wstring program = normalizedPath(toWide(programPath));
    bool allowMatch = false, blockMatch = false;
    VARIANT item;
    VariantInit(&item);
    ULONG fetched = 0;
    while (enumerator->Next(1, &item, &fetched) == S_OK && fetched == 1) {
        ComRef<INetFwRule> rule;
        if (item.vt == VT_DISPATCH && item.pdispVal) item.pdispVal->QueryInterface(kIidNetFwRule, rule.put());
        VariantClear(&item);
        if (!rule) continue;

        VARIANT_BOOL enabled = VARIANT_FALSE;
        if (FAILED(rule->get_Enabled(&enabled)) || enabled != VARIANT_TRUE) continue;
        NET_FW_RULE_DIRECTION direction = NET_FW_RULE_DIR_OUT;
        if (FAILED(rule->get_Direction(&direction)) || direction != NET_FW_RULE_DIR_IN) continue;
        long ruleProfiles = 0;
        if (FAILED(rule->get_Profiles(&ruleProfiles)) || !(ruleProfiles & profiles)) continue;
        long protocol = 0;
        if (FAILED(rule->get_Protocol(&protocol)) || (protocol != 17 && protocol != NET_FW_IP_PROTOCOL_ANY)) continue;
        BstrRef service;
        if (SUCCEEDED(rule->get_ServiceName(&service.s)) && service.s && *service.s && std::wcscmp(service.s, L"*") != 0) continue;
        BstrRef application;
        if (FAILED(rule->get_ApplicationName(&application.s))) continue;
        if (application.s && *application.s && normalizedPath(application.s) != program) continue;
        BstrRef ports;
        if (FAILED(rule->get_LocalPorts(&ports.s)) || !portListMatches(ports.s, port)) continue;

        NET_FW_ACTION action = NET_FW_ACTION_ALLOW;
        if (FAILED(rule->get_Action(&action))) continue;
        if (action == NET_FW_ACTION_BLOCK) blockMatch = true;
        else allowMatch = true;
    }

    status.blockRule = blockMatch;
    // Windows Firewall applies block rules before allow rules.
    status.allowed = blockMatch ? "no" : (allowMatch || defaultAllowsAll) ? "yes" : "no";
#else
    (void)port;
    (void)programPath;
#endif
    return status;
}

std::string firewallStatusJson(uint16_t port, const std::string& programPath) {
    std::string out;
    (void)glz::write_json(firewallStatus(port, programPath), out);
    return out;
}

std::string firewallRuleName(const std::string& programPath) {
    // The file name after the last separator of either kind, so a Windows path
    // reads the same on every platform.
    const auto slash = programPath.find_last_of("\\/");
    const std::string file = slash == std::string::npos ? programPath : programPath.substr(slash + 1);
    return file.empty() ? std::string("Track N Race") : "Track N Race (" + file + ")";
}

#ifdef _WIN32
namespace {
// A PowerShell single-quoted string literal.
std::wstring powershellQuoted(const std::wstring& text) {
    std::wstring quoted = L"'";
    for (wchar_t c : text) {
        quoted += c;
        if (c == L'\'') quoted += L'\'';
    }
    return quoted + L"'";
}
} // namespace
#endif

std::string firewallAllowProgram(const std::string& programPath) {
#ifdef _WIN32
    if (programPath.empty()) return "No program path was given.";
    // Only rules whose program is this executable are touched: its inbound
    // block rules, and the Track N Race allow rules added for it before (named
    // plain "Track N Race" by earlier versions). The other app's rule stays.
    const std::wstring script =
        L"$ErrorActionPreference='Stop';try{$p=" + powershellQuoted(toWide(programPath)) +
        L";$n=" + powershellQuoted(toWide(firewallRuleName(programPath))) + L";"
        L"$own=@(Get-NetFirewallApplicationFilter -Program $p -ErrorAction SilentlyContinue|Get-NetFirewallRule|"
        L"Where-Object{$_.Direction -eq 'Inbound'});"
        L"$own|Where-Object{$_.Action -eq 'Block' -or $_.DisplayName -like 'Track N Race*'}|Remove-NetFirewallRule;"
        L"New-NetFirewallRule -DisplayName $n -Direction Inbound -Action Allow -Program $p -Profile Any|Out-Null;"
        L"exit 0}catch{exit 1}";
    const std::wstring parameters =
        L"-NoProfile -NonInteractive -ExecutionPolicy Bypass -WindowStyle Hidden -EncodedCommand " + base64Utf16(script);

    ComScope com(COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    info.lpVerb = L"runas";
    info.lpFile = L"powershell.exe";
    info.lpParameters = parameters.c_str();
    info.nShow = SW_HIDE;
    if (!ShellExecuteExW(&info)) {
        const DWORD error = GetLastError();
        if (error == ERROR_CANCELLED) return "cancelled";
        return "Could not start the elevated firewall step (error " + std::to_string(error) + ").";
    }
    if (!info.hProcess) return {};
    const DWORD wait = WaitForSingleObject(info.hProcess, 120000);
    DWORD exitCode = 1;
    GetExitCodeProcess(info.hProcess, &exitCode);
    CloseHandle(info.hProcess);
    if (wait == WAIT_TIMEOUT) return "Timed out waiting for the firewall change.";
    return exitCode == 0 ? std::string() : "Windows could not add the firewall rule.";
#else
    (void)programPath;
    return "Firewall changes are only supported on Windows.";
#endif
}

} // namespace tnrp
