#include "tnrp/NetworkSetup.h"

#include <cassert>
#include <string>

int main() {
    using tnrp::firewallRuleName;
    // Each executable gets its own rule, so Electron and Qt don't replace each other's.
    assert(firewallRuleName(R"(C:\Program Files\Track N Race\Track-N-Race - Electron.exe)") ==
           "Track N Race (Track-N-Race - Electron.exe)");
    assert(firewallRuleName(R"(C:\Program Files\Track N Race - Qt\Track-N-Race - Qt.exe)") ==
           "Track N Race (Track-N-Race - Qt.exe)");
    assert(firewallRuleName(R"(C:\Program Files\Track N Race\Track-N-Race - Electron.exe)") !=
           firewallRuleName(R"(C:\Program Files\Track N Race - Qt\Track-N-Race - Qt.exe)"));
    assert(firewallRuleName("/opt/track-n-race/track-n-race") == "Track N Race (track-n-race)");
    assert(firewallRuleName("D:/dev/electron.exe") == "Track N Race (electron.exe)");
    assert(firewallRuleName("plain.exe") == "Track N Race (plain.exe)");
    assert(firewallRuleName("") == "Track N Race");
    assert(firewallRuleName("C:\dir\\") == "Track N Race");
    return 0;
}
