#include "tnrp/AnalysisMapPaint.h"

#include <cassert>
#include <string>

int main() {
    using namespace tnrp;

    // Ramps run from their start colour to their end colour.
    const AnalysisMapPalette& dark = analysisMapPalette(true);
    const AnalysisMapPalette& light = analysisMapPalette(false);
    assert(dark.ers.colors.size() == kAnalysisMapLevelSteps + 1);
    assert(dark.speed.colors.size() == kAnalysisMapSpeedSteps + 1);
    assert(dark.ers.colors.front() == "#3b528b");
    assert(dark.ers.colors.back() == "#7cf5d0");
    assert(light.harvest.colors.front() == "#4a3b2a");
    assert(light.harvest.colors.back() == "#ffb224");
    assert(dark.inputs.size() == 4 && dark.inputLabels.size() == 4);
    assert(analysisMapBlend("#3B528B", "#7CF5D0", 0.0) == "#3b528b");

    const std::string json = analysisMapPaletteJson();
    assert(json.rfind("{\"dark\":{", 0) == 0);
    assert(json.find("\"light\":{") != std::string::npos);

    // ERS Usage: a level picks its step on the ramp, by bin.
    AnalysisMapResult ers;
    ers.mode = "ers";
    ers.hasCurrent = true;
    ers.current.binM = 10.0f;
    ers.current.trackLengthM = 30.0f;
    ers.current.level = {0.0f, 0.5f, 1.0f};
    AnalysisMapPaint paint = analysisMapPaint(ers, 0, true);
    assert(paint.colors.size() == 3);
    assert(paint.colorAt(0.0) == dark.ers.colors.front());
    assert(paint.colorAt(0.5) == dark.ers.colors[kAnalysisMapLevelSteps / 2]);
    assert(paint.colorAt(0.99) == dark.ers.colors.back());
    assert(paint.colorAt(1.5) == dark.ers.colors.back());
    // No comparison lap, and a Restricted car, paint nothing.
    assert(analysisMapPaint(ers, 1, true).colors.empty());
    ers.current.restricted = true;
    assert(analysisMapPaint(ers, 0, true).colors.empty());

    // Track Dominance: the faster lap's colour, level in the theme's line colour.
    AnalysisMapResult dominance;
    dominance.mode = "dominance";
    dominance.hasCurrent = dominance.hasComparison = true;
    dominance.dominance.trackLengthM = 400.0f;
    dominance.dominance.faster = {0, 1, -1, 0};
    paint = analysisMapPaint(dominance, 0, false, "#111111", "#222222");
    assert(paint.colorAt(0.1) == "#111111");
    assert(paint.colorAt(0.3) == "#222222");
    assert(paint.colorAt(0.6) == light.level);
    assert(analysisMapPaint(dominance, 1, false, "#111111", "#222222").colors.empty());

    // ERS Harvest marks superclipping in its own colour.
    AnalysisMapResult harvest;
    harvest.mode = "harvest";
    harvest.hasCurrent = true;
    harvest.currentHarvest.binM = 10.0f;
    harvest.currentHarvest.trackLengthM = 20.0f;
    harvest.currentHarvest.level = {0.1f, 0.1f};
    harvest.currentHarvest.superclip = {0, 1};
    paint = analysisMapPaint(harvest, 0, true);
    assert(paint.colors[1] == dark.superclip);
    assert(paint.colors[0] != dark.superclip);

    // Speed spans the shared scale.
    AnalysisMapResult speed;
    speed.mode = "speed";
    speed.hasComparison = true;
    speed.comparisonSpeed.binM = 5.0f;
    speed.comparisonSpeed.trackLengthM = 10.0f;
    speed.comparisonSpeed.speedKph = {100.0f, 300.0f};
    speed.speedScaleMinKph = 100.0f;
    speed.speedScaleMaxKph = 300.0f;
    paint = analysisMapPaint(speed, 1, true);
    assert(paint.colors.front() == dark.speed.colors.front());
    assert(paint.colors.back() == dark.speed.colors.back());

    // Driver Inputs follow the pedal state.
    AnalysisMapResult inputs;
    inputs.mode = "inputs";
    inputs.hasCurrent = true;
    inputs.currentInputs.binM = 5.0f;
    inputs.currentInputs.trackLengthM = 20.0f;
    inputs.currentInputs.state = {0, 1, 2, 3};
    paint = analysisMapPaint(inputs, 0, true);
    for (size_t i = 0; i < 4; ++i) assert(paint.colors[i] == dark.inputs[i]);
    return 0;
}
