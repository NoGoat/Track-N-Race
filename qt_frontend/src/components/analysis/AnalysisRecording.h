#pragma once

#include "../../SessionModel.h"

#include <QHash>
#include <QPair>
#include <QString>
#include <QVector>

#include <cstdint>
#include <memory>

#include <tnrp/control_rows.h>

struct PlaybackHistoryBatch;

// Row-family bits shared with the playback engine's lap-data requests.
namespace AnalysisRows {
constexpr uint32_t Telemetry = 1u << 1;
constexpr uint32_t Status    = 1u << 2;
constexpr uint32_t Damage    = 1u << 3;
constexpr uint32_t Progress  = 1u << 4;
constexpr uint32_t Motion    = 1u << 11;
constexpr uint32_t MotionEx  = 1u << 12;
constexpr uint32_t Positions = 1u << 13;
}

// Identifies one driver in one of the two recordings Analysis can hold open.
// driverIndex may legitimately be -1 ("Recorded driver" in pre-V6 files), so
// validity is carried separately.
struct AnalysisDriverRef {
    bool valid = false;
    bool secondary = false;
    int driverIndex = -1;

    bool operator==(const AnalysisDriverRef&) const = default;
};

struct AnalysisLapRef {
    AnalysisDriverRef driver;
    int lapNum = -1;

    bool isValid() const { return driver.valid && lapNum > 0; }
    bool operator==(const AnalysisLapRef&) const = default;
};

// One recording's lap catalog for every analysable driver, plus a small LRU of
// laps that have been materialised with full-rate rows on demand.
class AnalysisRecording {
public:
    struct Driver {
        int index = -1;
        QString name;
        bool isPlayer = false;
        SessionData catalog;              // slim per-lap rows + lap/sector helpers
        QHash<int, LapBlock> laps;        // materialised laps, keyed by lap number
        QHash<int, uint32_t> installed;   // row families present per lap
        QHash<int, uint32_t> requested;   // row families in flight per lap
    };

    QString filename;
    QString path;
    QString trackName;
    int trackId = -1;
    uint64_t generation = 0;
    bool distanceAvailable = false;

    void setCatalog(const tnrp::PlaybackLapBlocksRow& catalog);

    const QVector<int>& driverOrder() const { return order_; }
    Driver* driver(int index);
    const Driver* driver(int index) const;

    // Fully materialised lap, or null until its rows arrive.
    const LapBlock* loadedLap(int driverIndex, int lapNum) const;
    // Slim catalog lap: always available once the catalog is set.
    const LapBlock* catalogLap(int driverIndex, int lapNum) const;

    // Row families of `wanted` neither installed nor in flight. They are marked
    // in flight, so the caller must request exactly what this returns.
    uint32_t claimMissingRows(int driverIndex, int lapNum, uint32_t wanted);

    // Installs a lap-data reply. Returns true when the visible data changed.
    bool install(int driverIndex, int lapNum, uint32_t requestedMask,
                 const std::shared_ptr<PlaybackHistoryBatch>& batch);

private:
    static constexpr int kLoadedLapLimit = 6;

    QHash<int, Driver> drivers_;
    QVector<int> order_;
    QVector<QPair<int, int>> recent_;   // (driver, lap), least recent first

    void touch(int driverIndex, int lapNum);
};
