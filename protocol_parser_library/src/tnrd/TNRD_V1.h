#pragma once

#include <memory>
#include <string>

#include "tnrp/control_rows.h"

namespace tnrp::detail { class TnrdOutputStream; }
namespace tnrp::detail::TNRD_V1 {

struct LoadResult {
    HeaderRow header;
    std::string tempPath;
    bool partial{};
};

bool load(const std::string& path, LoadResult& result, std::string& error);
void prepareHeader(HeaderRow& header);
std::unique_ptr<TnrdOutputStream> openWriter(const std::string& path, bool append,
                                             std::string& error);

} // namespace tnrp::detail::TNRD_V1
