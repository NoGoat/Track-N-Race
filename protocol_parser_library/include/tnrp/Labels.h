#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

// Library-owned, protocol-aware i18n label catalog.
//
// Every user-facing enum→string label (tyre compounds, ERS modes, fuel mix,
// flags, weather, session types, penalties, event codes, …) and the static UI
// chrome live here as a single source of truth, keyed by a stable id such as
// "ers.mode.3" or "drs.label". The catalog is layered: a base layer holds the
// format-neutral defaults; per-format override layers patch the keys whose
// meaning changes between game years (e.g. ERS deploy mode 3 is "Overtake" in
// 2025 but "Boost" in 2026; the DRS concept surfaces as "Straight Line Mode"
// in 2026).
//
// Consumers:
//   * native Qt recorder — calls labelsFor(format) in-process.
//   * Electron renderer  — receives labelsJson(format) embedded in the
//     protocol_status control row and exposes it via a t(key) hook.

namespace tnrp {

struct LabelCatalog {
    // Returns the label for `key`, or `key` itself if unknown (so a missing
    // entry is visible rather than blank).
    std::string get(std::string_view key) const;
    const std::unordered_map<std::string, std::string>& all() const { return map_; }

    std::unordered_map<std::string, std::string> map_;
};

// Resolved catalog (base overlaid with the format's overrides) for the given
// packet format (2024/2025/2026). Built once per format and cached.
const LabelCatalog& labelsFor(uint16_t format);

// The resolved catalog for `format` serialised as a flat JSON object
// {"key":"value",...}. Used to ship the catalog to the Electron renderer.
std::string labelsJson(uint16_t format);

} // namespace tnrp
