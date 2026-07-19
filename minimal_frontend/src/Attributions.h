#pragma once

#include <span>
#include <string>
#include <string_view>

struct Attribution {
    std::string_view name;
    std::string_view version;
    std::string_view license;
    std::string_view copyright;
    std::string_view website;
    std::string_view licenseText;
};

std::span<const Attribution> minimalAppAttributions();
std::string minimalAppAttributionDocument();
