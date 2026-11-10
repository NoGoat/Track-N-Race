#include "tnrp/AnalysisMapPaint.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace tnrp {

namespace {

constexpr double kPi = 3.14159265358979323846;

using Oklab = std::array<double, 3>;

int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

Oklab hexToOklab(const std::string& hex) {
    std::array<double, 3> rgb{};
    for (int i = 0; i < 3; ++i) {
        const size_t at = 1 + static_cast<size_t>(i) * 2;
        const int byte = hex.size() >= at + 2 ? hexDigit(hex[at]) * 16 + hexDigit(hex[at + 1]) : 0;
        const double c = byte / 255.0;
        rgb[i] = c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
    }
    const auto [r, g, b] = rgb;
    const double l = std::cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b);
    const double m = std::cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b);
    const double s = std::cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b);
    return {
        0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s,
        1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
        0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s,
    };
}

std::string oklabToHex(const Oklab& lab) {
    const auto [L, a, b] = lab;
    const double l = std::pow(L + 0.3963377774 * a + 0.2158037573 * b, 3);
    const double m = std::pow(L - 0.1055613458 * a - 0.0638541728 * b, 3);
    const double s = std::pow(L - 0.0894841775 * a - 1.2914855480 * b, 3);
    const std::array<double, 3> linear{
        4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s,
        -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s,
        -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s,
    };
    char out[8];
    int bytes[3];
    for (int i = 0; i < 3; ++i) {
        const double c = linear[i];
        const double v = c <= 0.0031308 ? 12.92 * c : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055;
        bytes[i] = static_cast<int>(std::lround(std::clamp(v, 0.0, 1.0) * 255.0));
    }
    std::snprintf(out, sizeof out, "#%02x%02x%02x", bytes[0], bytes[1], bytes[2]);
    return out;
}

// The colour `t` of the way from `from` to `to` in OKLCH, along the shorter
// hue arc.
std::string blend(const Oklab& from, const Oklab& to, double t) {
    const double c0 = std::hypot(from[1], from[2]);
    const double c1 = std::hypot(to[1], to[2]);
    const double h0 = std::atan2(from[2], from[1]);
    const double dh = std::fmod(std::atan2(to[2], to[1]) - h0 + 3 * kPi, 2 * kPi) - kPi;
    const double c = c0 + (c1 - c0) * t;
    const double h = h0 + dh * t;
    return oklabToHex({from[0] + (to[0] - from[0]) * t, c * std::cos(h), c * std::sin(h)});
}

AnalysisMapRamp ramp(const char* from, const char* to, int steps) {
    AnalysisMapRamp out{from, to, {}};
    const Oklab a = hexToOklab(out.from);
    const Oklab b = hexToOklab(out.to);
    out.colors.reserve(static_cast<size_t>(steps) + 1);
    for (int step = 0; step <= steps; ++step)
        out.colors.push_back(blend(a, b, static_cast<double>(step) / steps));
    return out;
}

AnalysisMapPalette makePalette(bool dark) {
    AnalysisMapPalette p;
    // ERS Usage: navy to aqua-mint on the dark theme, ocean blue to coral on
    // the light one.
    p.ers = dark ? ramp("#3B528B", "#7CF5D0", kAnalysisMapLevelSteps)
                 : ramp("#3D6FB6", "#FF7A59", kAnalysisMapLevelSteps);
    // ERS Harvest never looks like ERS Usage: wine to pink, brown to amber.
    p.harvest = dark ? ramp("#5E2A4F", "#FF8FC7", kAnalysisMapLevelSteps)
                     : ramp("#4A3B2A", "#FFB224", kAnalysisMapLevelSteps);
    // Speed: raspberry to periwinkle on the dark theme, olive to periwinkle on
    // the light one.
    p.speed = dark ? ramp("#C8577F", "#767AC9", kAnalysisMapSpeedSteps)
                   : ramp("#7E7F3C", "#767AC9", kAnalysisMapSpeedSteps);
    // Periwinkle sits apart from both harvest ramps.
    p.superclip = "#8C9CF0";
    // Red, amber and green read as brake, part throttle and flat out.
    p.inputs = dark ? std::vector<std::string>{"#8B8D98", "#FFB224", "#30A46C", "#E5484D"}
                    : std::vector<std::string>{"#80838D", "#C77700", "#218358", "#CE2C31"};
    p.inputLabels = {"Coasting", "Partial", "Full throttle", "Braking"};
    p.level = dark ? "#ffffff" : "#000000";
    return p;
}

// Per-bin levels (0-1) along a ramp; empty for a Restricted car.
AnalysisMapPaint levelPaint(bool restricted, const std::vector<float>& level, float binM,
                            float trackLengthM, const AnalysisMapRamp& ramp) {
    AnalysisMapPaint out{binM, trackLengthM, {}};
    if (restricted || level.empty() || !(binM > 0.0f)) return out;
    const int steps = static_cast<int>(ramp.colors.size()) - 1;
    out.colors.reserve(level.size());
    for (const float value : level)
        out.colors.push_back(ramp.colors[static_cast<size_t>(
            std::lround(std::clamp(static_cast<double>(value), 0.0, 1.0) * steps))]);
    return out;
}

} // namespace

const AnalysisMapPalette& analysisMapPalette(bool dark) {
    static const AnalysisMapPalette darkPalette = makePalette(true);
    static const AnalysisMapPalette lightPalette = makePalette(false);
    return dark ? darkPalette : lightPalette;
}

std::string analysisMapPaletteJson() {
    std::string out = "{\"dark\":";
    std::string part;
    (void)glz::write_json(analysisMapPalette(true), part);
    out += part;
    part.clear();
    (void)glz::write_json(analysisMapPalette(false), part);
    out += ",\"light\":";
    out += part;
    out += '}';
    return out;
}

std::string analysisMapBlend(const std::string& from, const std::string& to, double t) {
    return blend(hexToOklab(from), hexToOklab(to), std::clamp(t, 0.0, 1.0));
}

const std::string& AnalysisMapPaint::colorAt(double fraction) const {
    static const std::string none;
    if (colors.empty()) return none;
    const double n = static_cast<double>(colors.size());
    // binM 0: the colours are spread evenly over the lap.
    const double bin = binM > 0.0f && trackLengthM > 0.0f
        ? std::floor(fraction * trackLengthM / binM)
        : std::floor(fraction * n);
    return colors[static_cast<size_t>(std::clamp(bin, 0.0, n - 1))];
}

AnalysisMapPaint analysisMapPaint(const AnalysisMapResult& result, int lap, bool dark,
                                  const std::string& currentColor,
                                  const std::string& comparisonColor) {
    const AnalysisMapPalette& palette = analysisMapPalette(dark);
    const bool current = lap == 0;
    if (current ? !result.hasCurrent : !result.hasComparison) return {};

    if (result.mode == "dominance") {
        AnalysisMapPaint out;
        if (!current) return out;
        out.trackLengthM = result.dominance.trackLengthM;
        out.colors.reserve(result.dominance.faster.size());
        for (const int faster : result.dominance.faster)
            out.colors.push_back(faster == 0 ? currentColor : faster == 1 ? comparisonColor : palette.level);
        return out;
    }
    if (result.mode == "ers") {
        const ErsUsageMap& usage = current ? result.current : result.comparison;
        return levelPaint(usage.restricted, usage.level, usage.binM, usage.trackLengthM, palette.ers);
    }
    if (result.mode == "harvest") {
        const ErsHarvestMap& harvest = current ? result.currentHarvest : result.comparisonHarvest;
        AnalysisMapPaint out = levelPaint(harvest.restricted, harvest.level, harvest.binM,
                                          harvest.trackLengthM, palette.harvest);
        for (size_t bin = 0; bin < out.colors.size() && bin < harvest.superclip.size(); ++bin)
            if (harvest.superclip[bin]) out.colors[bin] = palette.superclip;
        return out;
    }
    if (result.mode == "inputs") {
        const DriverInputsMap& inputs = current ? result.currentInputs : result.comparisonInputs;
        AnalysisMapPaint out{inputs.binM, inputs.trackLengthM, {}};
        if (inputs.state.empty() || !(inputs.binM > 0.0f) || !(inputs.trackLengthM > 0.0f)) return out;
        out.colors.reserve(inputs.state.size());
        for (const int state : inputs.state)
            out.colors.push_back(state >= 0 && state < static_cast<int>(palette.inputs.size())
                ? palette.inputs[static_cast<size_t>(state)] : palette.inputs[0]);
        return out;
    }
    if (result.mode == "speed") {
        const SpeedMap& speed = current ? result.currentSpeed : result.comparisonSpeed;
        AnalysisMapPaint out{speed.binM, speed.trackLengthM, {}};
        if (speed.speedKph.empty() || !(speed.binM > 0.0f) || !(speed.trackLengthM > 0.0f)) return out;
        const double minKph = result.speedScaleMinKph;
        const double span = result.speedScaleMaxKph - minKph;
        const int steps = static_cast<int>(palette.speed.colors.size()) - 1;
        out.colors.reserve(speed.speedKph.size());
        for (const float kph : speed.speedKph) {
            const long step = span > 0.0
                ? std::lround(std::clamp((kph - minKph) / span, 0.0, 1.0) * steps) : steps;
            out.colors.push_back(palette.speed.colors[static_cast<size_t>(step)]);
        }
        return out;
    }
    return {};
}

} // namespace tnrp
