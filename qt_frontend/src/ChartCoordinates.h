#pragma once

#include "ChartSettings.h"
#include "SessionModel.h"

#include <QStringList>
#include <QVector>

// Resolved coordinate state for one graph section. Time/AL/SL keys are session
// time; CL/PL/FL/RL keys are lap distance, matching Electron. The current lap is
// clipped to the live/playback playhead while a completed comparison lap remains
// complete and is projected onto the same distance axis.
struct ChartDomain {
    ChartWindow window = ChartWindow::Seconds30;
    const LapBlock* primary = nullptr;
    const LapBlock* reference = nullptr;
    double lower = 0;
    double upper = 30;
    // Playback data is pre-scanned, so a LapBlock may contain samples after the
    // playhead. Current-lap rows must stop here; comparison laps remain complete.
    float currentTime = 0;
    bool distance = false;
    bool comparison = false;
    QVector<double> ticks;
    QStringList tickLabels;
};

ChartDomain resolveChartDomain(const SessionData& data, ChartWindow window,
                               int selectedLap, float currentTime,
                               bool sectorBoundaries,
                               const LapBlock* primaryLap,
                               const LapBlock* referenceLap);
double projectReferenceTime(const SessionData& data, const ChartDomain& domain,
                            float referenceSessionTime);
QVector<TelSample> chartTelSamples(const SessionData& data, const ChartDomain& domain);
QVector<StsSample> chartStatusSamples(const SessionData& data, const ChartDomain& domain);
QVector<TyreSample> chartTyreSamples(const SessionData& data, const ChartDomain& domain);
QVector<DamageSample> chartDamageSamples(const SessionData& data, const ChartDomain& domain);
QVector<MotionSample> chartMotionSamples(const SessionData& data, const ChartDomain& domain);
QVector<MotionExSample> chartMotionExSamples(const SessionData& data, const ChartDomain& domain);
