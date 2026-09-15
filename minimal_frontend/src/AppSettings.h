#pragma once

#include <cstdint>
#include <string>

#include <tnrp/Config.h>

struct AppSettings {
    std::string outputFolder;
    std::string bindAddress{"0.0.0.0"};
    uint16_t port{20777};
    std::string forwardTargets;
    tnrp::Override protocol{tnrp::Override::Auto};
};
