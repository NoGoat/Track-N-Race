#pragma once

#include <string_view>

namespace tnrp {

// Human-readable names shared by every host. Returned views refer to static
// storage and remain valid for the lifetime of the process.
std::string_view circuitName(int trackId) noexcept;
std::string_view sessionName(int sessionType) noexcept;

} // namespace tnrp
