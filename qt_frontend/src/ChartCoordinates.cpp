#include "ChartCoordinates.h"
#include "SessionModel.h"

#include <QtMath>

ChartDomain resolveChartDomain(const SessionData& data, ChartWindow window,
                               int selectedLap, float currentTime,
                               bool sectorBoundaries,
                               const LapBlock* primaryLap,
                               const LapBlock* referenceLap) {
    Q_UNUSED(selectedLap);
    ChartDomain out;
    out.window = window;
    out.currentTime = currentTime;
    if (chartWindowIsTime(window)) {
        const double seconds = chartWindowSeconds(window);
        out.lower = qMax(0.0, currentTime - seconds);
        out.upper = qMax(out.lower + 1.0, double(currentTime));
        return out;
    }
    if (window == ChartWindow::AllLaps || window == ChartWindow::StintLaps) {
        out.lower = window == ChartWindow::StintLaps ? data.currentStintStartTime : 0.0;
        if (!data.laps.isEmpty()) out.lower = qMax<double>(out.lower, data.laps.first().startSessionTime);
        out.upper = qMax(out.lower + 1.0, double(currentTime));
        for (const LapBlock& lap : data.laps) {
            if (lap.startSessionTime < out.lower || lap.startSessionTime > out.upper) continue;
            out.ticks.push_back(lap.startSessionTime);
            out.tickLabels.push_back(QString::number(lap.lapNum));
        }
        if (data.curLapNum >= 0 && data.curLap.startSessionTime >= out.lower) {
            out.ticks.push_back(data.curLap.startSessionTime);
            out.tickLabels.push_back(QString::number(data.curLap.lapNum));
        }
        return out;
    }

    out.distance = true;
    out.primary = primaryLap;
    if (!out.primary || out.primary->progress.isEmpty()) {
        out.window = ChartWindow::Seconds30;
        out.distance = false;
        out.lower = qMax(0.0, currentTime - 30.0);
        out.upper = qMax(out.lower + 1.0, double(currentTime));
        return out;
    }
    if (chartWindowIsComparison(window)) out.reference = referenceLap;
    // Electron keeps the requested lap-relative coordinate mode even while a
    // previous/fastest/reference lap is not available yet. It simply omits the
    // comparison trace; it does not silently fall back to a 30-second chart.
    out.comparison = chartWindowIsComparison(window) && out.reference && !out.reference->progress.isEmpty();
    const double length = qMax<double>(data.trackLengthM,
        qMax(out.primary->progress.isEmpty() ? 0.0 : double(out.primary->progress.last().distanceM),
             out.reference && !out.reference->progress.isEmpty() ? double(out.reference->progress.last().distanceM) : 0.0));
    out.lower = 0.0;
    out.upper = qMax(1.0, length);
    if (sectorBoundaries) {
        out.ticks.push_back(0.0);
        out.tickLabels.push_back(QString());
        for (const auto& split : data.sectorSplits(out.primary)) {
            out.ticks.push_back(split.distanceM);
            out.tickLabels.push_back(QString("S%1").arg(split.sector));
        }
        out.ticks.push_back(length);
        out.tickLabels.push_back("S3");
    } else if (length > 0) {
        for (int i = 0; i <= 4; ++i) {
            const double distance = length * i / 4.0;
            out.ticks.push_back(distance);
            out.tickLabels.push_back(QString("%1 m").arg(qRound(distance)));
        }
    }
    return out;
}

double projectReferenceTime(const SessionData& data, const ChartDomain& domain,
                            float referenceSessionTime) {
    if (!domain.comparison) return qQNaN();
    return data.distanceAtTime(domain.reference, referenceSessionTime);
}

namespace {
template <typename Sample>
QVector<Sample> collectSamples(const SessionData& data, const ChartDomain& domain,
                               const QVector<Sample>& buffer,
                               QVector<Sample> LapBlock::* member) {
    QVector<Sample> result;
    auto appendRange = [&](const QVector<Sample>& source) {
        for (const Sample& sample : source)
            if (sample.t >= domain.lower && sample.t <= domain.upper) result.push_back(sample);
    };
    if (domain.distance && domain.primary) {
        const QVector<Sample>& source = domain.primary->*member;
        for (const Sample& sample : source) {
            if (sample.t > domain.currentTime) break;
            result.push_back(sample);
        }
    }
    else appendRange(buffer);
    return result;
}
}

QVector<TelSample> chartTelSamples(const SessionData& d, const ChartDomain& c) {
    return collectSamples(d, c, d.telBuf, &LapBlock::tel);
}
QVector<StsSample> chartStatusSamples(const SessionData& d, const ChartDomain& c) {
    return collectSamples(d, c, d.stsBuf, &LapBlock::sts);
}
QVector<TyreSample> chartTyreSamples(const SessionData& d, const ChartDomain& c) {
    return collectSamples(d, c, d.tyreBuf, &LapBlock::tyre);
}
QVector<DamageSample> chartDamageSamples(const SessionData& d, const ChartDomain& c) {
    return collectSamples(d, c, d.damageBuf, &LapBlock::damage);
}
QVector<MotionSample> chartMotionSamples(const SessionData& d, const ChartDomain& c) {
    return collectSamples(d, c, d.motionBuf, &LapBlock::motion);
}
QVector<MotionExSample> chartMotionExSamples(const SessionData& d, const ChartDomain& c) {
    return collectSamples(d, c, d.motionExBuf, &LapBlock::motionEx);
}
