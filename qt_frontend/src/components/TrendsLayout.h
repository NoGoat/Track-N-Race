#pragma once

#include <QString>

// Settings ▸ Layout ▸ Trends ▸ Chart Layout (Electron TrendsPageLayout):
// four separate charts, one combined chart with a scale per unit (with or
// without recharge), or three bars per lap.
enum class TrendsChartLayout { Separate, Combined, CombinedNoRecharge, Bars };

inline QString trendsChartLayoutKey(TrendsChartLayout layout) {
    switch (layout) {
        case TrendsChartLayout::Combined:           return QStringLiteral("combined");
        case TrendsChartLayout::CombinedNoRecharge: return QStringLiteral("combinedNoRecharge");
        case TrendsChartLayout::Bars:               return QStringLiteral("bars");
        default:                                    return QStringLiteral("separate");
    }
}

inline TrendsChartLayout trendsChartLayoutFromKey(const QString& key) {
    if (key == QLatin1String("combined")) return TrendsChartLayout::Combined;
    if (key == QLatin1String("combinedNoRecharge")) return TrendsChartLayout::CombinedNoRecharge;
    if (key == QLatin1String("bars")) return TrendsChartLayout::Bars;
    return TrendsChartLayout::Separate;
}

// Electron's TrendsLayout (appConfig.ts): the summary cards, then the
// Separate layout's four charts and the single chart of the combined and bar
// layouts, each shown or hidden in Edit Layout.
struct TrendsLayout {
    enum StatCard { StintLaps, WearPerLap, RecPerLap, ErsPerLap, FuelPerLap, FastestLap, PreviousLap, Tyre, StatCount };
    enum Chart { LapTimes, ErsUsage, Recharge, TyreWear, Combined, Bars, ChartCount };

    bool statsCards[StatCount] = {true, true, true, true, true, true, true, true};
    bool charts[ChartCount] = {true, true, true, true, true, true};

    static const char* statKey(int idx) {
        static const char* keys[StatCount] = {
            "stintLaps", "wearPerLap", "recPerLap", "ersPerLap", "fuelPerLap", "fastestLap", "previousLap", "tyre"
        };
        return keys[idx];
    }
    static const char* statLabel(int idx) {
        static const char* labels[StatCount] = {
            "Stint Laps", "Wear/Lap", "Rec/Lap", "ERS/Lap", "Fuel/Lap", "Fastest Lap", "Previous Lap", "Tyre"
        };
        return labels[idx];
    }
    static const char* chartKey(int idx) {
        static const char* keys[ChartCount] = {
            "lapTimes", "ersUsage", "recharge", "tyreWear", "combined", "bars"
        };
        return keys[idx];
    }
};
