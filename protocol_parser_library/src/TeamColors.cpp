#include "tnrp/TeamColors.h"

#include <algorithm>
#include <cctype>

#include <glaze/glaze.hpp>

#include "tnrp/control_rows.h"

namespace tnrp {
namespace {

using Colors = std::vector<TeamColor>;

const Colors kF124 = {
    {0, "Mercedes", "#27F4D2"}, {1, "Ferrari", "#E8002D"},
    {2, "Red Bull Racing", "#3671C6"}, {3, "Williams", "#64C4FF"},
    {4, "Aston Martin", "#229971"}, {5, "Alpine", "#FF87BC"},
    {6, "RB", "#6692FF"}, {7, "Haas", "#B6BABD"},
    {8, "McLaren", "#FF8000"}, {9, "Sauber", "#52E252"},
    {143, "ART Grand Prix '23", "#B4B3B4"},
    {144, "Campos Racing '23", "#EFC100"},
    {145, "Carlin '23", "#243EF6"},
    {146, "PHM Racing '23", "#84020A"},
    {147, "DAMS '23", "#0ED4FA"},
    {148, "Hitech Pulse-Eight '23", "#E8E8E8"},
    {149, "MP Motorsport '23", "#F7401A"},
    {150, "Prema Racing '23", "#E80309"},
    {151, "Trident '23", "#0E1185"},
    {152, "Van Amersfoort Racing '23", "#F36F21"},
    {153, "Virtuosi Racing '23", "#FBEC20"},
};

const Colors kF125 = {
    {0, "Mercedes", "#00D7B6"}, {1, "Ferrari", "#ED1131"},
    {2, "Red Bull Racing", "#4781D7"}, {3, "Williams", "#1868DB"},
    {4, "Aston Martin", "#229971"}, {5, "Alpine", "#00A1E8"},
    {6, "RB", "#6C98FF"}, {7, "Haas", "#9C9FA2"},
    {8, "McLaren", "#F47600"}, {9, "Sauber", "#01C00E"},
    {158, "ART Grand Prix '24", "#B4B3B4"},
    {159, "Campos Racing '24", "#EFC100"},
    {160, "Rodin Motorsport '24", "#FFFFFF"},
    {161, "AIX Racing '24", "#FF6900"},
    {162, "DAMS '24", "#0ED4FA"},
    {163, "Hitech Pulse-Eight '24", "#E8E8E8"},
    {164, "MP Motorsport '24", "#F7401A"},
    {165, "Prema Racing '24", "#E80309"},
    {166, "Trident '24", "#0E1185"},
    {167, "Van Amersfoort Racing '24", "#F36F21"},
    {168, "Invicta Racing '24", "#F8E71C"},
    {185, "Mercedes '24", "#27F4D2"}, {186, "Ferrari '24", "#E8002D"},
    {187, "Red Bull Racing '24", "#3671C6"}, {188, "Williams '24", "#64C4FF"},
    {189, "Aston Martin '24", "#229971"}, {190, "Alpine '24", "#FF87BC"},
    {191, "RB '24", "#6692FF"}, {192, "Haas '24", "#B6BABD"},
    {193, "McLaren '24", "#FF8000"}, {194, "Sauber '24", "#52E252"},
};

const Colors kF126 = {
    {0, "Mercedes", "#00D7B6"}, {1, "Ferrari", "#ED1131"},
    {2, "Red Bull Racing", "#4781D7"}, {3, "Williams", "#1868DB"},
    {4, "Aston Martin", "#229971"}, {5, "Alpine", "#00A1E8"},
    {6, "RB", "#6C98FF"}, {7, "Haas", "#9C9FA2"},
    {8, "McLaren", "#F47600"}, {9, "Sauber", "#01C00E"},
    {158, "ART Grand Prix '24", "#B4B3B4"},
    {159, "Campos Racing '24", "#EFC100"},
    {160, "Rodin Motorsport '24", "#FFFFFF"},
    {161, "AIX Racing '24", "#FF6900"},
    {162, "DAMS '24", "#0ED4FA"},
    {163, "Hitech Pulse-Eight '24", "#E8E8E8"},
    {164, "MP Motorsport '24", "#F7401A"},
    {165, "Prema Racing '24", "#E80309"},
    {166, "Trident '24", "#0E1185"},
    {167, "Van Amersfoort Racing '24", "#F36F21"},
    {168, "Invicta Racing '24", "#F8E71C"},
    {185, "Mercedes '24", "#27F4D2"}, {186, "Ferrari '24", "#E8002D"},
    {187, "Red Bull Racing '24", "#3671C6"}, {188, "Williams '24", "#64C4FF"},
    {189, "Aston Martin '24", "#229971"}, {190, "Alpine '24", "#FF87BC"},
    {191, "RB '24", "#6692FF"}, {192, "Haas '24", "#B6BABD"},
    {193, "McLaren '24", "#FF8000"}, {194, "Sauber '24", "#52E252"},
    {465, "ART Grand Prix '25", "#B4B3B4"},
    {466, "Campos Racing '25", "#EFC100"},
    {467, "Rodin Motorsport '25", "#FFFFFF"},
    {468, "AIX Racing '25", "#FF6900"},
    {469, "DAMS '25", "#0ED4FA"},
    {470, "Hitech Pulse-Eight '25", "#E8E8E8"},
    {471, "MP Motorsport '25", "#F7401A"},
    {472, "Prema Racing '25", "#E80309"},
    {473, "Trident '25", "#0E1185"},
    {474, "Van Amersfoort Racing '25", "#F36F21"},
    {475, "Invicta Racing '25", "#F8E71C"},
    {476, "Mercedes '26", "#27F4D2"}, {477, "Ferrari '26", "#E8002D"},
    {478, "Red Bull Racing '26", "#3671C6"}, {479, "Williams '26", "#1868DB"},
    {480, "Aston Martin '26", "#229971"}, {481, "Alpine '26", "#00A1E8"},
    {482, "RB '26", "#6692FF"}, {483, "Haas '26", "#DEE1E2"},
    {484, "McLaren '26", "#FF8000"}, {485, "Audi '26", "#FF2D00"},
    {486, "Cadillac '26", "#AAAAAD"},
    {489, "ART Grand Prix '26", "#B4B3B4"},
    {490, "Campos Racing '26", "#EFC100"},
    {491, "Rodin Motorsport '26", "#FFFFFF"},
    {492, "AIX Racing '26", "#FF6900"},
    {493, "DAMS '26", "#0ED4FA"},
    {494, "Hitech '26", "#E8E8E8"},
    {495, "MP Motorsport '26", "#F7401A"},
    {496, "Prema Racing '26", "#E80309"},
    {497, "Trident '26", "#0E1185"},
    {498, "Van Amersfoort Racing '26", "#F36F21"},
    {499, "Invicta Racing '26", "#F8E71C"},
};

bool isHexColor(std::string_view color) {
    return color.size() == 7 && color.front() == '#' &&
        std::all_of(color.begin() + 1, color.end(), [](unsigned char c) {
            return std::isxdigit(c) != 0;
        });
}

std::string normalizedHex(std::string_view color) {
    if (!isHexColor(color)) return {};
    std::string result(color);
    std::transform(result.begin() + 1, result.end(), result.begin() + 1,
        [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return result;
}

const TeamColor* findPreset(uint16_t format, uint16_t teamId) {
    const auto& colors = teamColorsFor(format);
    const auto it = std::find_if(colors.begin(), colors.end(), [teamId](const TeamColor& team) {
        return team.id == teamId;
    });
    return it == colors.end() ? nullptr : &*it;
}

std::pair<int, const char*> groupFor(uint16_t format, uint16_t id) {
    if (format == 2024) {
        return id <= 9
            ? std::pair{0, "F1 2024 Teams"}
            : std::pair{1, "F2 2023 Teams"};
    }
    if (format == 2025) {
        if (id <= 9) return {0, "F1 2025 Teams"};
        if (id >= 185) return {1, "F1 2024 Teams"};
        return {2, "F2 2024 Teams"};
    }
    if (id >= 476 && id <= 486) return {0, "F1 2026 Teams"};
    if (id <= 9) return {1, "F1 2025 Teams"};
    if (id >= 185 && id <= 194) return {2, "F1 2024 Teams"};
    if (id >= 489 && id <= 499) return {3, "F2 2026 Teams"};
    if (id >= 465) return {4, "F2 2025 Teams"};
    return {5, "F2 2024 Teams"};
}

Colors catalogForUi(uint16_t format) {
    Colors result = teamColorsFor(format);
    for (auto& team : result) team.group = groupFor(format, team.id).second;
    std::stable_sort(result.begin(), result.end(), [format](const TeamColor& a, const TeamColor& b) {
        const int aGroup = groupFor(format, a.id).first;
        const int bGroup = groupFor(format, b.id).first;
        return aGroup != bGroup ? aGroup < bGroup : a.id < b.id;
    });
    return result;
}

} // namespace

const std::vector<TeamColor>& teamColorsFor(uint16_t format) {
    static const Colors empty;
    if (format == 2024) return kF124;
    if (format == 2025) return kF125;
    if (format == 2026) return kF126;
    return empty;
}

std::string teamColorCatalogJson() {
    std::string out = "{\"2024\":";
    std::string part;
    (void)glz::write_json(catalogForUi(2024), part);
    out += part;
    out += ",\"2025\":";
    part.clear();
    (void)glz::write_json(catalogForUi(2025), part);
    out += part;
    out += ",\"2026\":";
    part.clear();
    (void)glz::write_json(catalogForUi(2026), part);
    out += part;
    out += '}';
    return out;
}

TeamColorOverrides sanitizeTeamColorOverrides(const TeamColorOverrides& overrides) {
    TeamColorOverrides result;
    for (const auto& [format, teams] : overrides) {
        if (format != 2024 && format != 2025 && format != 2026) continue;
        for (const auto& [teamId, color] : teams) {
            if (!findPreset(format, teamId)) continue;
            if (color == "livery" && (format == 2025 || format == 2026)) {
                result[format][teamId] = color;
                continue;
            }
            std::string normalized = normalizedHex(color);
            if (!normalized.empty()) result[format][teamId] = std::move(normalized);
        }
    }
    return result;
}

std::string resolveTeamColor(uint16_t format, uint16_t teamId,
                             std::string_view packetColor,
                             const TeamColorOverrides& overrides) {
    const auto formatIt = overrides.find(format);
    if (formatIt != overrides.end()) {
        const auto teamIt = formatIt->second.find(teamId);
        if (teamIt != formatIt->second.end()) {
            if (teamIt->second == "livery") {
                if (format == 2025 || format == 2026)
                    return isHexColor(packetColor) ? normalizedHex(packetColor) : "#8E8E8E";
            } else if (isHexColor(teamIt->second)) {
                return normalizedHex(teamIt->second);
            }
        }
    }
    if (const TeamColor* preset = findPreset(format, teamId)) return preset->color;
    if (isHexColor(packetColor)) return normalizedHex(packetColor);
    return "#8E8E8E";
}

void applyTeamColorToDriver(Driver& driver, uint16_t format,
                            const TeamColorOverrides& overrides) {
    if ((format == 2025 || format == 2026) && !driver.source_livery_color)
        driver.source_livery_color = driver.livery_color;
    driver.livery_color = resolveTeamColor(format, static_cast<uint16_t>(driver.team_id),
        driver.source_livery_color.value_or(driver.livery_color), overrides);
}

std::string applyTeamColorsToParticipantsJson(std::string_view json,
                                              uint16_t format,
                                              const TeamColorOverrides& overrides) {
    if (json.find("\"type\":\"participants\"") == std::string_view::npos) return std::string(json);
    constexpr glz::opts readOptions{ .null_terminated = false, .error_on_unknown_keys = false };
    ParticipantsRow row;
    if (glz::read<readOptions>(row, json)) return std::string(json);
    for (auto& driver : row.drivers) {
        applyTeamColorToDriver(driver, format, overrides);
    }
    std::string result;
    if (glz::write_json(row, result)) return std::string(json);
    return result;
}

} // namespace tnrp
