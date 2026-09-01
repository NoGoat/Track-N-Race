#include "tnrp/Parser.h"
#include "tnrp/Capabilities.h"

#include <cassert>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace {
void put16(std::vector<uint8_t>& packet, size_t offset, uint16_t value) {
    std::memcpy(packet.data() + offset, &value, sizeof(value));
}
void put32(std::vector<uint8_t>& packet, size_t offset, uint32_t value) {
    std::memcpy(packet.data() + offset, &value, sizeof(value));
}
void putFloat(std::vector<uint8_t>& packet, size_t offset, float value) {
    std::memcpy(packet.data() + offset, &value, sizeof(value));
}
}

int main() {
    assert(tnrp::presentationFormat(2026, std::nullopt) == 2026);
    assert(tnrp::presentationFormat(2026, 13) == 2026);
    assert(tnrp::presentationFormat(2026, 0) == 2025);
    assert(tnrp::presentationFormat(2025, 13) == 2025);

    const std::string missingFormula = tnrp::Parser::statusRowForFormat(2026);
    assert(missingFormula.find("\"active_format\":2026") != std::string::npos);
    assert(missingFormula.find("\"presentation_format\":2026") != std::string::npos);
    assert(missingFormula.find("\"aero_mode\":\"slm\"") != std::string::npos);
    const std::string legacyFormula = tnrp::Parser::statusRowForFormat(2026, 0);
    assert(legacyFormula.find("\"active_format\":2026") != std::string::npos);
    assert(legacyFormula.find("\"presentation_format\":2025") != std::string::npos);
    assert(legacyFormula.find("\"hasMguh\":true") != std::string::npos);
    assert(legacyFormula.find("\"aero_mode\":\"drs\"") != std::string::npos);

    std::vector<uint8_t> legacySession(926, 0);
    put16(legacySession, 0, 2026);
    legacySession[2] = 26;
    legacySession[5] = 1;
    legacySession[6] = 1;
    legacySession[37] = 0;
    tnrp::Parser formulaParser(tnrp::Override::F1_26);
    const auto legacyResult = formulaParser.feed(legacySession.data(),
        static_cast<int>(legacySession.size()), "2026-08-16T00:00:00Z", false, 0);
    assert(legacyResult.control.size() == 1);
    assert(legacyResult.control[0].find("\"presentation_format\":2025") != std::string::npos);
    legacySession[37] = 13;
    const auto modernResult = formulaParser.feed(legacySession.data(),
        static_cast<int>(legacySession.size()), "2026-08-16T00:00:01Z", false, 0);
    assert(modernResult.control.size() == 1);
    assert(modernResult.control[0].find("\"presentation_format\":2026") != std::string::npos);

    const struct Case { uint16_t format; tnrp::Override overrideValue; } cases[] = {
        {2024, tnrp::Override::F1_24},
        {2025, tnrp::Override::F1_25},
        {2026, tnrp::Override::F1_26},
    };
    for (const auto& test : cases) {
        std::vector<uint8_t> packet(45, 0);
        put16(packet, 0, test.format);
        packet[2] = static_cast<uint8_t>(test.format - 2000);
        packet[5] = 1;
        packet[6] = 3;
        putFloat(packet, 15, 100.0f);
        put32(packet, 19, 9000);
        put32(packet, 23, 12000); // deliberately monotonic; not the rewind target
        std::memcpy(packet.data() + 29, "FLBK", 4);
        put32(packet, 33, 8000);
        putFloat(packet, 37, 42.5f);

        tnrp::Parser parser(test.overrideValue);
        const auto result = parser.feed(packet.data(), static_cast<int>(packet.size()),
                                        "2026-08-16T00:00:00Z", false, 0);
        assert(!result.dropped);
        assert(result.rewindSessionTime && std::fabs(*result.rewindSessionTime - 42.5f) < 0.001f);
        assert(result.rewindFrameIdentifier && *result.rewindFrameIdentifier == 8000);
        assert(result.rows.size() == 1); // delivered even with the event family unsubscribed
        assert(result.rows[0].find("\"code\":\"FLBK\"") != std::string::npos);
        assert(result.rows[0].find("\"flashback_frame_identifier\":8000") != std::string::npos);
        assert(result.rows[0].find("\"flashback_session_time\":42.5") != std::string::npos);

        const int carCount = test.format == 2026 ? 24 : 22;
        constexpr int headerSize = 29;
        constexpr int lapSize = 57;
        std::vector<uint8_t> lapPacket(headerSize + carCount * lapSize + 2, 0);
        put16(lapPacket, 0, test.format);
        lapPacket[2] = static_cast<uint8_t>(test.format - 2000);
        lapPacket[5] = 1;
        lapPacket[6] = 2; // Lap Data
        lapPacket[7] = 1;
        putFloat(lapPacket, 15, 50.0f);
        lapPacket[27] = 0;
        lapPacket[headerSize + 33] = 4; // current lap number
        lapPacket[headerSize + 44] = 3; // out lap

        tnrp::Parser lapParser(test.overrideValue);
        const auto lapResult = lapParser.feed(lapPacket.data(), static_cast<int>(lapPacket.size()),
                                              "2026-08-16T00:00:00Z", false, 1u << 4);
        assert(!lapResult.dropped);
        assert(lapResult.rows.size() == 1);
        assert(lapResult.rows[0].find("\"driver_status\":3") != std::string::npos);
    }
    return 0;
}
