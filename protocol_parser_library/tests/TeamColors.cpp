#include "tnrp/Parser.h"
#include "tnrp/TeamColors.h"
#include "tnrp/control_rows.h"

#include <cassert>
#include <cstring>
#include <string>
#include <vector>

namespace {
tnrp::ParticipantsRow readParticipants(const std::string& json) {
    tnrp::ParticipantsRow row;
    const auto error = glz::read_json(row, json);
    assert(!error);
    return row;
}
}

int main() {
    using namespace tnrp;
    const auto sanitized = sanitizeTeamColorOverrides({
        {2024, {{0, "livery"}, {1, "#abcdef"}}},
        {2025, {{0, "livery"}, {1, "invalid"}, {65535, "livery"}}},
        {2026, {{489, "livery"}, {499, "#abcdef"}}},
    });
    assert(sanitized.at(2024).count(0) == 0);
    assert(sanitized.at(2024).at(1) == "#ABCDEF");
    assert(sanitized.at(2025).size() == 1);
    assert(sanitized.at(2025).at(0) == "livery");
    assert(sanitized.at(2026).at(489) == "livery");
    assert(sanitized.at(2026).at(499) == "#ABCDEF");
    assert(resolveTeamColor(2024, 0, "#123456", {{2024, {{0, "livery"}}}}) == "#27F4D2");

    for (const uint16_t format : {2025, 2026}) {
        const TeamColorOverrides livery{{format, {{0, "livery"}}}};
        const TeamColorOverrides fixed{{format, {{0, "#AABBCC"}}}};
        assert(resolveTeamColor(format, 0, "#123abc", livery) == "#123ABC");
        assert(resolveTeamColor(format, 0, "", livery) == "#8E8E8E");
        assert(resolveTeamColor(format, 104, "#123abc", {}) == "#123ABC");

        // Independently constructed Participants packets from the documented
        // 57-byte (2025) / 60-byte (2026) layouts. The first RGB is at name+38.
        const size_t cars = format == 2025 ? 22 : 24;
        const size_t stride = format == 2025 ? 57 : 60;
        const size_t nameOffset = 30 + (format == 2025 ? 7 : 10);
        std::vector<uint8_t> packet(30 + cars * stride, 0);
        packet[0] = static_cast<uint8_t>(format & 0xff);
        packet[1] = static_cast<uint8_t>(format >> 8);
        packet[2] = static_cast<uint8_t>(format - 2000);
        packet[5] = 1;
        packet[6] = 4;
        packet[29] = 1;
        std::memcpy(packet.data() + nameOffset, "Player", 7);
        packet[nameOffset + 37] = 1;
        packet[nameOffset + 38] = 0x12;
        packet[nameOffset + 39] = 0x3a;
        packet[nameOffset + 40] = 0xbc;
        Parser parser(format == 2025 ? Override::F1_25 : Override::F1_26, fixed);
        const auto result = parser.feed(packet.data(), static_cast<int>(packet.size()), "", false);
        assert(result.rows.size() == 1);
        auto row = readParticipants(result.rows.front());
        assert(row.drivers.size() == 1);
        assert(row.drivers[0].livery_color == "#AABBCC");
        assert(row.drivers[0].source_livery_color == "#123abc");

        // Repeated changes and JSON round trips must never substitute a preset
        // or custom color for the original packet color.
        std::string json = result.rows.front();
        for (int i = 0; i < 3; ++i) {
            json = applyTeamColorsToParticipantsJson(json, format, livery);
            row = readParticipants(json);
            assert(row.drivers[0].livery_color == "#123ABC");
            assert(row.drivers[0].source_livery_color == "#123abc");
            json = applyTeamColorsToParticipantsJson(json, format, fixed);
            assert(readParticipants(json).drivers[0].livery_color == "#AABBCC");
        }
        // Two cars on the same team retain distinct game livery colors.
        Driver teammate{1, "Teammate", 0, 2, false, "#AABBCC", "#fedcba"};
        applyTeamColorToDriver(teammate, format, livery);
        assert(teammate.livery_color == "#FEDCBA");

        // Old recordings omit the source field: preserve their recorded color
        // as a stable fallback across subsequent settings changes.
        const std::string legacy = R"({"type":"participants","drivers":[{"idx":0,"name":"Player","team_id":0,"race_number":1,"ai":false,"livery_color":"#112233"}]})";
        json = applyTeamColorsToParticipantsJson(legacy, format, fixed);
        json = applyTeamColorsToParticipantsJson(json, format, livery);
        assert(readParticipants(json).drivers[0].livery_color == "#112233");
    }
}
