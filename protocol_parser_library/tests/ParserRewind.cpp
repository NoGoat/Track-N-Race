#include "tnrp/Parser.h"

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
