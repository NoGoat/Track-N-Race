#pragma once

#include <string>
#include <vector>
#include "protocol.h"

namespace F1_26 {
    std::vector<std::string> ParsePacket(const uint8_t* data, int length, const PacketHeader& hdr, const std::string& timestamp, HotOut& hot);
}
