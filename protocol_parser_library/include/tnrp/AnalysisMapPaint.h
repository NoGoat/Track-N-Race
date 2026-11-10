#pragma once

#include <string>
#include <vector>

#include <glaze/glaze.hpp>

#include "tnrp/AnalysisMap.h"

// Colours for the Analysis map layers (tnrp/AnalysisMap.h). The ramps blend in
// OKLCH the short way round, as CSS `in oklch` gradients do, so middle values
// stay coloured rather than grey. Colours are "#rrggbb".

namespace tnrp {

// Steps per ramp: ERS Usage and ERS Harvest, then Speed, whose finer steps
// keep a few km/h between neighbouring colours.
constexpr int kAnalysisMapLevelSteps = 20;
constexpr int kAnalysisMapSpeedSteps = 50;

struct AnalysisMapRamp {
    std::string from;
    std::string to;
    // steps + 1 colours from `from` to `to`.
    std::vector<std::string> colors;
};

// One theme's colours for every layer.
struct AnalysisMapPalette {
    AnalysisMapRamp ers;       // ERS Usage: no deployment to the cap
    AnalysisMapRamp harvest;   // ERS Harvest: nothing to the car's peak harvest
    AnalysisMapRamp speed;     // Speed: slowest to fastest on show
    std::string superclip;     // ERS Harvest: superclipping, at full strength
    // Driver Inputs, indexed by PedalState: coasting, partial, full throttle, braking.
    std::vector<std::string> inputs;
    std::vector<std::string> inputLabels;
    std::string level;         // Track Dominance: neither lap faster
};

const AnalysisMapPalette& analysisMapPalette(bool dark);
// {"dark": AnalysisMapPalette, "light": AnalysisMapPalette}
std::string analysisMapPaletteJson();

// The colour at `t` (0-1) along a ramp's start and end, blended in OKLCH.
std::string analysisMapBlend(const std::string& from, const std::string& to, double t);

// One lap's map painted: a colour per stretch of `binM` metres from the timing
// line. Empty colours when there is nothing to paint: a Restricted car's ERS,
// Track Dominance without a comparison lap, or no data.
struct AnalysisMapPaint {
    float binM{};
    float trackLengthM{};
    std::vector<std::string> colors;

    // The colour at `fraction` (0-1) of the lap from the timing line; empty
    // when the paint is empty.
    const std::string& colorAt(double fraction) const;
};

// What `result.mode` paints on a lap's map: `lap` 0 for the current lap, 1 for
// the comparison lap. Track Dominance paints only lap 0, in the faster lap's
// colour (`currentColor` / `comparisonColor`) per mini-sector.
AnalysisMapPaint analysisMapPaint(const AnalysisMapResult& result, int lap, bool dark,
                                  const std::string& currentColor = {},
                                  const std::string& comparisonColor = {});

} // namespace tnrp

template <>
struct glz::meta<tnrp::AnalysisMapRamp> {
    using T = tnrp::AnalysisMapRamp;
    static constexpr auto value = glz::object("from", &T::from, "to", &T::to, "colors", &T::colors);
};

template <>
struct glz::meta<tnrp::AnalysisMapPalette> {
    using T = tnrp::AnalysisMapPalette;
    static constexpr auto value = glz::object(
        "ers",         &T::ers,
        "harvest",     &T::harvest,
        "speed",       &T::speed,
        "superclip",   &T::superclip,
        "inputs",      &T::inputs,
        "inputLabels", &T::inputLabels,
        "level",       &T::level);
};
