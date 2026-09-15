#pragma once

struct InputLayout {
    bool showGear = true;
    bool showAccelerator = true;
    bool showBrake = true;
    bool showSteering = true;
};

enum class InputPageLayout { Grid, Vertical };
enum class InputPedalLayout { Combined, Combined2, Split };
