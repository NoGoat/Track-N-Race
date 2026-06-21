#pragma once

#include <array>

// Hide/show state for the Overview page's cards and chart, persisted via
// QSettings (see MainWindow::loadOverviewLayout/saveOverviewLayout). Index
// order matches the order cards are built in OverviewPage.cpp, since the
// same index also addresses MainWindow's ov_statCardFrame_/ov_dmgCardFrame_
// arrays.
struct OverviewLayout {
    bool showChart  = true;
    enum TyreView { TyreCards, TyreCharts };
    TyreView tyreView = TyreCards;

    // Per-corner card visibility (FL=0, FR=1, RL=2, RR=3)
    static constexpr int TyreCornerCount = 4;
    std::array<bool, TyreCornerCount> tyreCardVisible  = { true, true, true, true };
    // Per-chart visibility (Surface=0, Inner=1, Brake=2, Wear=3)
    static constexpr int TyreChartCount = 4;
    std::array<bool, TyreChartCount> tyreChartVisible = { true, true, true, true };

    enum StatCard { Speed, Rpm, Gear, Throttle, Brake, Drs, EngineTemp, Ers, Fuel, Pos, Tyre, StatCardCount };
    std::array<bool, StatCardCount> statCards = {
        true, true, true, true, true, true, true, true, true, true, true,
    };

    enum DmgCard {
        TyreFl, TyreFr, TyreRl, TyreRr, BrakeFl, BrakeFr, BrakeRl, BrakeRr,
        WingFl, WingFr, WingRear, Floor, Sidepod, Diffuser, Gearbox, Engine,
        DmgCardCount
    };
    std::array<bool, DmgCardCount> dmgCards = {
        false, false, false, false, false, false, false, false,
        true,  true,  true,  true,  true,  true,  true,  true,
    };

    static const char* statCardLabel(int i);
    static const char* dmgCardLabel(int i);
    static const char* statCardKey(int i);
    static const char* dmgCardKey(int i);
    static const char* tyreCardLabel(int i);
    static const char* tyreCardKey(int i);
    static const char* tyreChartLabel(int i);
    static const char* tyreChartKey(int i);
};
