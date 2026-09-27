#include "AnalysisRecording.h"

#include "../../TnrdPlayer.h"

#include <algorithm>

namespace {

void fillCatalog(AnalysisRecording::Driver& driver, int index, const QString& name,
                 bool isPlayer, const std::vector<tnrp::LapBlockMeta>& blocks,
                 const std::vector<tnrp::LapMeta>& lapTimes, int fastestLapNum,
                 int trackLengthM) {
    driver.index = index;
    driver.name = name.isEmpty() ? QStringLiteral("Car %1").arg(index) : name;
    driver.isPlayer = isPlayer;
    driver.catalog.clear();
    driver.catalog.trimBuffers = false;
    driver.catalog.fastestLapNum = fastestLapNum;
    driver.catalog.trackLengthM = static_cast<float>(trackLengthM);

    QHash<int, int> times;
    for (const auto& lap : lapTimes) times.insert(lap.lapNum, lap.lapTimeMs);

    driver.catalog.laps.reserve(static_cast<qsizetype>(blocks.size()));
    for (const auto& source : blocks) {
        LapBlock lap;
        lap.lapNum = source.lapNum;
        lap.startSessionTime = source.startSessionTime;
        lap.endSessionTime = source.endSessionTime;
        lap.lapTimeMs = times.value(source.lapNum);
        lap.tel.reserve(static_cast<qsizetype>(source.telemetry.size()));
        for (const auto& point : source.telemetry) {
            TelSample sample;
            sample.t = point.session_time;
            sample.speed = static_cast<float>(point.speed_kph);
            sample.rpm = static_cast<float>(point.rpm);
            sample.gear = sample.throttle = sample.brake = sample.steering = float(qQNaN());
            lap.tel.push_back(sample);
        }
        lap.sts.reserve(static_cast<qsizetype>(source.statusHistory.size()));
        for (const auto& point : source.statusHistory) {
            StsSample sample;
            sample.t = point.session_time;
            sample.ers = static_cast<float>(point.ers_pct);
            sample.tyre_compound = point.tyre_compound;
            sample.visual_compound = point.visual_compound;
            lap.sts.push_back(sample);
        }
        driver.catalog.laps.push_back(std::move(lap));
    }
    std::sort(driver.catalog.laps.begin(), driver.catalog.laps.end(),
              [](const LapBlock& a, const LapBlock& b) {
                  return a.startSessionTime < b.startSessionTime;
              });
}

// Appends rows that arrived in a later reply for the same lap, keeping the
// series time-ordered and dropping exact duplicates at the seam.
template <class Sample>
void mergeRows(QVector<Sample>& target, QVector<Sample>&& source) {
    if (target.isEmpty()) {
        target = std::move(source);
        return;
    }
    target += std::move(source);
    std::stable_sort(target.begin(), target.end(),
                     [](const Sample& a, const Sample& b) { return a.t < b.t; });
    const auto last = std::unique(target.begin(), target.end(),
                                  [](const Sample& a, const Sample& b) {
                                      return qFuzzyCompare(a.t + 1.0f, b.t + 1.0f);
                                  });
    target.erase(last, target.end());
}

} // namespace

void AnalysisRecording::setCatalog(const tnrp::PlaybackLapBlocksRow& catalog) {
    drivers_.clear();
    order_.clear();
    recent_.clear();
    distanceAvailable = catalog.lapDistanceAvailable || catalog.deltaAvailable;

    if (catalog.analysisDrivers.empty()) {
        // Pre-V6 recordings carry only the recorded driver's laps.
        Driver recorded;
        fillCatalog(recorded, -1, QStringLiteral("Recorded driver"), true, catalog.blocks,
                    catalog.laps, catalog.fastestLapNum, catalog.trackLengthM);
        order_.push_back(-1);
        drivers_.insert(-1, std::move(recorded));
        return;
    }
    for (const auto& source : catalog.analysisDrivers) {
        Driver driver;
        fillCatalog(driver, source.driverIndex, QString::fromStdString(source.driverName),
                    source.isPlayer, source.blocks, source.laps, source.fastestLapNum,
                    catalog.trackLengthM);
        order_.push_back(source.driverIndex);
        drivers_.insert(source.driverIndex, std::move(driver));
    }
}

AnalysisRecording::Driver* AnalysisRecording::driver(int index) {
    const auto it = drivers_.find(index);
    return it == drivers_.end() ? nullptr : &it.value();
}

const AnalysisRecording::Driver* AnalysisRecording::driver(int index) const {
    const auto it = drivers_.constFind(index);
    return it == drivers_.cend() ? nullptr : &it.value();
}

const LapBlock* AnalysisRecording::loadedLap(int driverIndex, int lapNum) const {
    const Driver* owner = driver(driverIndex);
    if (!owner) return nullptr;
    const auto it = owner->laps.constFind(lapNum);
    return it == owner->laps.cend() ? nullptr : &it.value();
}

const LapBlock* AnalysisRecording::catalogLap(int driverIndex, int lapNum) const {
    const Driver* owner = driver(driverIndex);
    return owner ? owner->catalog.lapByNum(lapNum) : nullptr;
}

uint32_t AnalysisRecording::claimMissingRows(int driverIndex, int lapNum, uint32_t wanted) {
    Driver* owner = driver(driverIndex);
    if (!owner || lapNum <= 0) return 0;
    const uint32_t missing = wanted & ~owner->installed.value(lapNum) &
                             ~owner->requested.value(lapNum);
    if (missing) owner->requested[lapNum] |= missing;
    return missing;
}

bool AnalysisRecording::install(int driverIndex, int lapNum, uint32_t requestedMask,
                                const std::shared_ptr<PlaybackHistoryBatch>& batch) {
    Driver* owner = driver(driverIndex);
    if (!owner) return false;
    owner->requested[lapNum] &= ~requestedMask;
    if (!batch || batch->lapDetails.isEmpty()) return false;

    LapBlock detail = batch->lapDetails.first();
    auto it = owner->laps.find(lapNum);
    if (it == owner->laps.end()) {
        LapBlock base;
        if (const LapBlock* meta = owner->catalog.lapByNum(lapNum)) {
            base = *meta;
        } else {
            base.lapNum = lapNum;
            base.startSessionTime = detail.startSessionTime;
            base.endSessionTime = detail.endSessionTime;
        }
        it = owner->laps.insert(lapNum, std::move(base));
    }

    LapBlock& lap = it.value();
    uint32_t& installed = owner->installed[lapNum];
    const uint32_t payload = batch->rowTypeMask;
    auto take = [&](uint32_t bit, auto& target, auto& source) {
        if (!(payload & bit)) return;
        // The first reply for a family replaces the slim catalog rows outright.
        if (installed & bit) mergeRows(target, std::move(source));
        else target = std::move(source);
        installed |= bit;
    };
    if (payload & AnalysisRows::Telemetry) {
        if (lap.tyre.isEmpty()) lap.tyre = std::move(detail.tyre);
        else mergeRows(lap.tyre, std::move(detail.tyre));
    }
    take(AnalysisRows::Telemetry, lap.tel, detail.tel);
    take(AnalysisRows::Status, lap.sts, detail.sts);
    take(AnalysisRows::Damage, lap.damage, detail.damage);
    take(AnalysisRows::Progress, lap.progress, detail.progress);
    take(AnalysisRows::Motion, lap.motion, detail.motion);
    take(AnalysisRows::MotionEx, lap.motionEx, detail.motionEx);
    take(AnalysisRows::Positions, lap.positions, detail.positions);

    touch(driverIndex, lapNum);
    return true;
}

void AnalysisRecording::touch(int driverIndex, int lapNum) {
    const QPair<int, int> key(driverIndex, lapNum);
    recent_.removeAll(key);
    recent_.push_back(key);
    while (recent_.size() > kLoadedLapLimit) {
        const QPair<int, int> evicted = recent_.takeFirst();
        if (Driver* owner = driver(evicted.first)) {
            owner->laps.remove(evicted.second);
            owner->installed.remove(evicted.second);
            owner->requested.remove(evicted.second);
        }
    }
}
