#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <glaze/glaze.hpp>

// Helpers for first-time setup: which addresses the game can send telemetry to,
// and whether the operating system's firewall lets that telemetry in. Shared by
// every frontend so each shows the same adapters and the same firewall verdict.

namespace tnrp {

// Kinds, in the order they are listed: real adapters first, loopback last.
enum class NetworkInterfaceKind { Wifi, Ethernet, Other, Virtual, Loopback };

const char* networkInterfaceKindName(NetworkInterfaceKind kind);

// Classifies an adapter from what the OS reports about it. `ifType` is the
// Windows IANA interface type (IF_TYPE_*), or -1 where there is none.
// `wireless` is true when the OS says the adapter is Wi-Fi.
NetworkInterfaceKind classifyNetworkInterface(std::string_view name, std::string_view description,
                                              int ifType, bool wireless);

struct NetworkInterfaceInfo {
    std::string name;     // OS adapter name (e.g. "Wi-Fi", "wlp3s0", "en0")
    std::string kind;     // networkInterfaceKindName(): wifi | ethernet | other | virtual | loopback
    std::string address;  // IPv4, dotted
};

// IPv4 addresses of adapters that are up, sorted by kind and then name.
// Link-local (169.254.x.x) addresses are left out. Exactly one loopback entry
// (127.0.0.1) is always present, last.
std::vector<NetworkInterfaceInfo> listNetworkInterfaces();

// listNetworkInterfaces() as JSON: [{"name","kind","address"}, …].
std::string networkInterfacesJson();

struct FirewallStatus {
    bool supported = false;       // false where the firewall cannot be queried
    bool enabled = false;         // on for at least one active network profile
    bool publicNetwork = false;   // an active network is set as Public
    bool privateNetwork = false;
    bool domainNetwork = false;
    std::string allowed = "unknown";  // "yes" | "no" | "unknown"
    bool blockRule = false;       // an inbound block rule matches this program or port
    std::string error;
};

// Whether inbound UDP on `port` reaches `programPath` under the active network
// profiles. Windows only: elsewhere `supported` is false and `allowed` unknown.
FirewallStatus firewallStatus(uint16_t port, const std::string& programPath);
std::string firewallStatusJson(uint16_t port, const std::string& programPath);

// The display name of the allow rule firewallAllowProgram() adds for
// `programPath`: "Track N Race (<executable file name>)", so the Electron and
// Qt apps (or two installs) each keep a rule of their own.
std::string firewallRuleName(const std::string& programPath);

// Windows: asks for elevation (UAC), removes inbound block rules for
// `programPath` and the Track N Race allow rules earlier versions added for it,
// then adds an inbound allow rule named firewallRuleName() for it on every
// profile. Rules for any other program are left alone. Blocks until the
// elevated step finishes. Returns an empty string on success, "cancelled" when
// the UAC prompt is declined, or an error. Elsewhere it returns an error
// without doing anything.
std::string firewallAllowProgram(const std::string& programPath);

} // namespace tnrp

template <>
struct glz::meta<tnrp::NetworkInterfaceInfo> {
    using T = tnrp::NetworkInterfaceInfo;
    static constexpr auto value = object("name", &T::name, "kind", &T::kind, "address", &T::address);
};

template <>
struct glz::meta<tnrp::FirewallStatus> {
    using T = tnrp::FirewallStatus;
    static constexpr auto value = object(
        "supported", &T::supported, "enabled", &T::enabled,
        "publicNetwork", &T::publicNetwork, "privateNetwork", &T::privateNetwork,
        "domainNetwork", &T::domainNetwork, "allowed", &T::allowed,
        "blockRule", &T::blockRule, "error", &T::error);
};
