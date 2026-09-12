#pragma once

#include "GraphViewSettings.h"

#include <QString>

// Ordinary-chart coordinate modes. Values are deliberately stable because they
// are persisted by name, never by combo-box index.
enum class ChartWindow {
    Seconds15, Seconds30, Seconds60, Seconds120, Seconds300, Seconds600,
    CurrentLap, PreviousLap, FastestLap, SelectedLap, StintLaps, AllLaps
};

inline QString chartWindowKey(ChartWindow w) {
    switch (w) {
        case ChartWindow::Seconds15:   return "15";
        case ChartWindow::Seconds30:   return "30";
        case ChartWindow::Seconds60:   return "60";
        case ChartWindow::Seconds120:  return "120";
        case ChartWindow::Seconds300:  return "300";
        case ChartWindow::Seconds600:  return "600";
        case ChartWindow::CurrentLap:  return "CL";
        case ChartWindow::PreviousLap: return "PL";
        case ChartWindow::FastestLap:  return "FL";
        case ChartWindow::SelectedLap: return "RL";
        case ChartWindow::StintLaps:   return "SL";
        case ChartWindow::AllLaps:     return "AL";
    }
    return "30";
}

inline ChartWindow chartWindowFromKey(const QString& key) {
    if (key == "15")  return ChartWindow::Seconds15;
    if (key == "60")  return ChartWindow::Seconds60;
    if (key == "120") return ChartWindow::Seconds120;
    if (key == "300") return ChartWindow::Seconds300;
    if (key == "600") return ChartWindow::Seconds600;
    if (key == "CL")  return ChartWindow::CurrentLap;
    if (key == "PL")  return ChartWindow::PreviousLap;
    if (key == "FL")  return ChartWindow::FastestLap;
    if (key == "RL")  return ChartWindow::SelectedLap;
    if (key == "SL")  return ChartWindow::StintLaps;
    if (key == "AL")  return ChartWindow::AllLaps;
    return ChartWindow::Seconds30;
}

inline QString chartWindowLabel(ChartWindow w) {
    switch (w) {
        case ChartWindow::Seconds15:   return "15s";
        case ChartWindow::Seconds30:   return "30s";
        case ChartWindow::Seconds60:   return "1m";
        case ChartWindow::Seconds120:  return "2m";
        case ChartWindow::Seconds300:  return "5m";
        case ChartWindow::Seconds600:  return "10m";
        case ChartWindow::CurrentLap:  return "Current";
        case ChartWindow::PreviousLap: return "Previous";
        case ChartWindow::FastestLap:  return "Fastest";
        case ChartWindow::SelectedLap: return "Selected";
        case ChartWindow::StintLaps:   return "Stint Laps";
        case ChartWindow::AllLaps:     return "All Laps";
    }
    return "30s";
}

inline bool chartWindowIsTime(ChartWindow w) {
    return w >= ChartWindow::Seconds15 && w <= ChartWindow::Seconds600;
}
inline bool chartWindowIsDistance(ChartWindow w) {
    return w >= ChartWindow::CurrentLap && w <= ChartWindow::SelectedLap;
}
inline bool chartWindowIsComparison(ChartWindow w) {
    return w == ChartWindow::PreviousLap || w == ChartWindow::FastestLap ||
           w == ChartWindow::SelectedLap;
}
inline bool chartWindowAccumulatesLaps(ChartWindow w) {
    return w == ChartWindow::StintLaps || w == ChartWindow::AllLaps;
}
inline float chartWindowSeconds(ChartWindow w) {
    switch (w) {
        case ChartWindow::Seconds15: return 15.0f;
        case ChartWindow::Seconds60: return 60.0f;
        case ChartWindow::Seconds120: return 120.0f;
        case ChartWindow::Seconds300: return 300.0f;
        case ChartWindow::Seconds600: return 600.0f;
        default: return 30.0f;
    }
}

inline QString chartWindowOverrideKey(tnr::GraphSection s) {
    return QString("ui/chartWindow/%1").arg(tnr::graphSectionId(s));
}
inline QString chartReferenceLapKey(tnr::GraphSection s) {
    return QString("ui/chartReferenceLap/%1").arg(tnr::graphSectionId(s));
}
inline QString chartYAxisKey(tnr::GraphSection s) {
    return QString("ui/chartYAxis/%1").arg(tnr::graphSectionId(s));
}
