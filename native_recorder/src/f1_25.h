#pragma once

#include "protocol.h"

namespace F1_25 {
    std::vector<nlohmann::json> ParsePacket(const uint8_t* data, int length, const PacketHeader& hdr, const std::string& timestamp);
}
