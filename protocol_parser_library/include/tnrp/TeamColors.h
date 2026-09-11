#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace tnrp {

// Library-owned F1/F2 constructor colour catalog. Hosts persist only the
// sparse overrides; team ids, display names, and preset colours stay here so
// every parser and playback path resolves them identically.
struct TeamColor {
    uint16_t    id{};
    std::string name;
    std::string color;
    std::string group;
};

using TeamColorOverrides = std::map<uint16_t, std::map<uint16_t, std::string>>;

const std::vector<TeamColor>& teamColorsFor(uint16_t format);
std::string teamColorCatalogJson();

// Invalid formats, ids, and colours are removed. Hex colours are normalized to
// uppercase #RRGGBB before they enter the parser.
TeamColorOverrides sanitizeTeamColorOverrides(const TeamColorOverrides& overrides);

// Resolution order: user override, library preset, packet livery colour,
// neutral fallback. Unknown/non-constructor ids therefore retain the game's
// dynamic livery colour on F1 25/26.
std::string resolveTeamColor(uint16_t format, uint16_t teamId,
                             std::string_view packetColor,
                             const TeamColorOverrides& overrides);

// Rewrites only participants rows. Used by playback so current overrides also
// apply to recordings created before (or with different) colour settings.
std::string applyTeamColorsToParticipantsJson(std::string_view json,
                                              uint16_t format,
                                              const TeamColorOverrides& overrides);

} // namespace tnrp
