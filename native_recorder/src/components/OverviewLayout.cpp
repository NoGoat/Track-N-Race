#include "OverviewLayout.h"

const char* OverviewLayout::statCardLabel(int i)
{
    static const char* labels[StatCardCount] = {
        "Speed", "RPM", "Gear", "Throttle", "Brake", "DRS",
        "Engine", "ERS", "Fuel", "Pos", "Tyre"
    };
    return labels[i];
}

const char* OverviewLayout::dmgCardLabel(int i)
{
    static const char* labels[DmgCardCount] = {
        "Tyre FL", "Tyre FR", "Tyre RL", "Tyre RR",
        "Brake FL", "Brake FR", "Brake RL", "Brake RR",
        "Wing FL", "Wing FR", "Wing Rear", "Floor",
        "Sidepod", "Diffuser", "Gearbox", "Engine"
    };
    return labels[i];
}

const char* OverviewLayout::statCardKey(int i)
{
    static const char* keys[StatCardCount] = {
        "speed", "rpm", "gear", "throttle", "brake", "drs",
        "engine", "ers", "fuel", "pos", "tyre"
    };
    return keys[i];
}

const char* OverviewLayout::dmgCardKey(int i)
{
    static const char* keys[DmgCardCount] = {
        "tyreFl", "tyreFr", "tyreRl", "tyreRr",
        "brakeFl", "brakeFr", "brakeRl", "brakeRr",
        "wingFl", "wingFr", "wingRear", "floor",
        "sidepod", "diffuser", "gearbox", "engine"
    };
    return keys[i];
}
