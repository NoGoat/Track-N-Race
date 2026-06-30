#pragma once

#include <map>
#include <string>
#include <vector>

#include <glaze/glaze.hpp>

// Library-owned, declarative card-color model — the colour counterpart to
// tnrp/Labels.h. Each card key maps to a ColorSpec: an unconditional `default`
// colour token plus an ordered list of conditional rules (first match wins). The
// rules reference either the card's own value ("self") or a named data field, so
// the same thresholds drive both the Electron renderer and the native recorder.
//
// Colours themselves are NOT here — only semantic *tokens* (pos/neg/warn/…). Each
// app maps tokens → real colours with its own theme-aware palette, so theming
// stays per-app while the rule structure is shared. Card colours do not vary by
// game year, so the spec is format-independent and fetched once.

namespace tnrp {

struct ColorRule {
    std::string on;     // "self" or a data field id (engine_temp, ers_pct, …)
    std::string op;     // "lt" | "lte" | "gt" | "gte" | "eq"
    double      value{};
    std::string color;  // token id (see palettes in each app)
};

struct ColorSpec {
    std::string            def;    // unconditional default token (JSON key "default")
    std::vector<ColorRule> rules;  // evaluated in order; first satisfied wins
};

// Per-card-key specs (sorted for deterministic JSON).
const std::map<std::string, ColorSpec>& cardColors();

// The spec table serialised as JSON: { "<key>": {"default":…,"rules":[…]}, … }.
std::string cardColorsJson();

} // namespace tnrp

template <>
struct glz::meta<tnrp::ColorRule> {
    using T = tnrp::ColorRule;
    static constexpr auto value = glz::object(
        "on",    &T::on,
        "op",    &T::op,
        "value", &T::value,
        "color", &T::color
    );
};

template <>
struct glz::meta<tnrp::ColorSpec> {
    using T = tnrp::ColorSpec;
    static constexpr auto value = glz::object(
        "default", &T::def,
        "rules",   &T::rules
    );
};
