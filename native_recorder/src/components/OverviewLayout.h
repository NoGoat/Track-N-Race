#pragma once

#include <array>

// Hide/show state for the Overview page's cards and chart, persisted via
// QSettings (see MainWindow::loadOverviewLayout/saveOverviewLayout). Index
// order matches the order cards are built in OverviewPage.cpp, since the
// same index also addresses MainWindow's ov_statCardFrame_/ov_dmgCardFrame_
// arrays.
struct OverviewLayout {
    bool showChart = true;

    enum StatCard { Speed, Rpm, Gear, Throttle, Brake, Drs, Engine, Ers, Fuel, Pos, Tyre, StatCardCount };
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
};
